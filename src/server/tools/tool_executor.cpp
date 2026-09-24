#include "minimal/tools/tool_executor.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "minimal/mcp/naming.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/tools/pattern_match.hpp"

namespace minimal::tools {
namespace {

llm::ToolResultBlock ErrorResult(const std::string& tool_use_id, std::string text) {
    llm::ToolResultBlock block;
    block.tool_use_id = tool_use_id;
    block.is_error = true;
    block.content.push_back(llm::TextBlock{std::move(text)});
    return block;
}

}  // namespace

std::string mcp_server_of(const std::string& tool_name) { return mcp::McpServerOf(tool_name); }

ToolExecutor::ToolExecutor(ToolRegistry& registry, PermissionChecker& checker,
                            PermissionPolicy policy, McpPermissionPolicy mcp_policy,
                            PermissionRunMode run_mode,
                            std::optional<PermissionPolicy> policy_ceiling,
                            std::optional<McpPermissionPolicy> mcp_ceiling,
                            std::vector<std::string> seed_grants,
                            const hooks::HookDispatcher* hooks)
    : registry_(registry),
      checker_(checker),
      policy_(std::move(policy)),
      mcp_policy_(std::move(mcp_policy)),
      run_mode_(run_mode),
      policy_ceiling_(std::move(policy_ceiling)),
      mcp_ceiling_(std::move(mcp_ceiling)),
      session_grants_(seed_grants.begin(), seed_grants.end()),
      hooks_(hooks) {}

namespace {

const char* CategoryName(ToolCategory category) {
    switch (category) {
        case ToolCategory::ReadOnly: return "read_only";
        case ToolCategory::FileWrite: return "file_write";
        case ToolCategory::Execute: return "execute";
    }
    return "read_only";
}

// The hook-payload label. MCP tools carry a placeholder ReadOnly category (the
// server-scoped McpPermissionPolicy governs them, never a category), so a
// category of "read_only" on an arbitrary remote call would mislead a
// `category` hook filter -- they are labelled "mcp".
const char* HookCategoryName(const std::string& tool_name, ToolCategory category) {
    return mcp_server_of(tool_name).empty() ? CategoryName(category) : "mcp";
}

}  // namespace

ToolExecutor::Verdict ToolExecutor::Explain(const Tool& tool, const nlohmann::json& input,
                                           const ToolContext& ctx) const {
    const std::string& name = tool.spec().name;  // surface name: what the model calls it
    const ToolCategory category = tool.category_for(input).value_or(tool.spec().category);

    // 1. Is this tool allowed at all? MCP tools resolve from their server's
    //    mode, refined by a per-tool rule on the full mcp__ name.
    //    The policy lookup uses the PERMISSION key (the non-alias name), never
    //    the surface: per_tool_overrides must keep governing a tool when an
    //    alias changes what the model sees. For unaliased tools the two agree.
    const std::string_view permission_key = registry_.PermissionKeyFor(name);
    const std::string mcp_server = mcp_server_of(name);

    // The pattern-matching subject, computed when any CONSUMING policy actually
    // carries pattern rules (the confine() call below is filesystem work; a
    // pattern-free pair must not pay it on every invocation). Consumers: this
    // executor's own pattern_overrides, the subagent ceiling's
    // pattern_overrides, and the ceiling's out_of_workdir_patterns -- the
    // ceiling's folds below all read this subject, so a pattern-free CHILD
    // policy must not suppress it when the PARENT's rules need matching (that
    // was the bug: the parent's {"git *": "ask"} silently never matched for a
    // child whose own preset declared no patterns). A canonical path anchors
    // patterns like "**/.env" at "/" for confinement tools; the raw subject
    // stands for everything else (bash's command line).
    // Path confinement, canonicalized at most ONCE: for a confinement-
    // supporting tool, PatternSubject() defaults to (and nothing in this
    // codebase overrides it away from) ExtractPath() -- see tool.hpp -- so
    // the pattern-subject use below and the out-of-workdir fold in step 2
    // always confine() the identical path. Computing it here once, ahead of
    // both, means a single canonicalization serves both instead of one per
    // use site.
    std::optional<std::string> confinable_path;
    std::optional<ConfineResult> confined;
    if (tool.path_confinement_supported()) {
        confinable_path = tool.ExtractPath(input);
        if (confinable_path) {
            confined = confine(ctx.project_root, *confinable_path, /*must_exist=*/false);
        }
    }

    PatternSubjects subjects;
    bool subject_is_path = false;
    const bool needs_pattern_subject =
        !policy_.pattern_overrides.empty() ||
        (policy_ceiling_.has_value() &&
         (!policy_ceiling_->pattern_overrides.empty() ||
          !policy_ceiling_->out_of_workdir_patterns.empty()));
    if (needs_pattern_subject) {
        if (tool.path_confinement_supported()) {
            if (confinable_path) {
                subjects.whole =
                    confined->ok ? confined->absolute_path.string() : *confinable_path;
                subject_is_path = true;
            }
        } else if (auto raw = tool.PatternSubject(input)) {
            subjects.whole = std::move(*raw);
        }
        // Stages ride along when the tool can decompose the subject. Only a
        // pattern-carrying policy pays for the decomposition (the same gate as
        // the subject above); a pattern-free pair never sees this work.
        subjects.stages = tool.PatternStages(input).value_or(std::vector<std::string>{});
        subjects.allow_eligible = tool.PatternAllowEligible(input);
        subjects.restrict_only = tool.PatternRestrictSubjects(input);
    }

    // `why` names the rule behind `mode`; every fold below that TIGHTENS the
    // mode replaces it, so it always explains the final answer.
    std::string why;
    PermissionMode mode =
        mcp_server.empty()
            ? policy_.mode_for(permission_key, category, subjects, subject_is_path, &why)
            : resolve_mcp_tool_mode(policy_, mcp_policy_, mcp_server, permission_key, &why);
    auto tighten = [&](PermissionMode candidate, auto&& describe) {
        if (candidate < mode) {
            mode = candidate;
            why = describe();
        }
    };
    auto dimension = [](std::string_view name, PermissionMode m) {
        return std::string(name) + "=" + std::string(mode_name(m));
    };

    // The confinement verdict of THIS invocation, consumed by the ceiling fold
    // below and surfaced to Prepare for the execute-time re-check.
    bool checked = false;
    bool inside = false;

    // 2. Only if permitted, and only for path-bearing tools, fold in
    //    out_of_workdir. This is a FOLD, never a substitution: substituting
    //    would mean `file_write: deny` + `out_of_workdir: allow` permits writing
    //    /etc/foo while denying ./foo.cpp -- exactly inverted. The dimension's
    //    PATTERN rules (out_of_workdir_patterns) replace the scalar for this
    //    invocation when any of them matches the canonical path: the operator's
    //    per-path intent is the more specific statement.
    if (mode != PermissionMode::AlwaysDeny && tool.path_confinement_supported()) {
        if (confinable_path) {
            const ConfineResult& resolved = *confined;
            checked = true;
            inside = resolved.ok && !resolved.outside_project_root;
            if (!inside) {
                // An unresolvable path is treated as outside: fail closed rather
                // than silently applying the in-workdir rule to something we
                // could not place.
                PermissionMode ood = policy_.out_of_workdir;
                std::string ood_why = "outside the project: " + dimension("out_of_workdir", ood);
                if (!policy_.out_of_workdir_patterns.empty() && resolved.ok) {
                    std::optional<PermissionMode> folded;
                    for (const auto& rule : policy_.out_of_workdir_patterns) {
                        if (GlobMatch(rule.glob, resolved.absolute_path.string(),
                                      /*slash_terminates=*/true) &&
                            (!folded || rule.mode < *folded)) {
                            folded = rule.mode;
                            ood_why = "outside the project: pattern `" + rule.glob + "`=" +
                                      std::string(mode_name(rule.mode));
                        }
                    }
                    if (folded) ood = *folded;
                }
                tighten(ood, [&] { return ood_why; });
            }
        }
    }

    // 2a. Same fold, same reason, for the network dimension: only if still
    //     permitted, and only for tools that declare network_access_required().
    if (mode != PermissionMode::AlwaysDeny && tool.network_access_required()) {
        tighten(policy_.network,
                [&] { return "network access: " + dimension("network", policy_.network); });
    }

    // 2b. Subagent ceiling, when one was handed down: a child is never more
    //     permissive than its parent. Folds like out_of_workdir -- it can only
    //     tighten -- and sits before run mode so the clamped run mode still
    //     wraps the whole decision.
    //
    //     Same key rule as the lookup above: the ceiling's per_tool_overrides
    //     are keyed by PERMISSION key (they come from the parent's resolved
    //     policy), so an aliased tool would miss its parent's deny if this
    //     matched on the surface name -- the child could run exactly the tools
    //     the parent's operator had renamed out of the deny list.
    if (mode != PermissionMode::AlwaysDeny) {
        if (mcp_server.empty()) {
            if (policy_ceiling_) {
                // The ceiling's own pattern rules ride along: a parent's
                // `{"git *": "ask"}` tightens the child's matching calls even
                // when the child preset asked for allow. Folding
                // most_restrictive keeps the ceiling one-directional -- its
                // pattern-allows can never widen the child.
                std::string ceiling_why;
                const PermissionMode ceiling_mode = policy_ceiling_->mode_for(
                    permission_key, category, subjects, subject_is_path, &ceiling_why);
                tighten(ceiling_mode, [&] { return "parent session: " + ceiling_why; });
                // out_of_workdir and network are dimension FIELDS, not
                // categories, so mode_for() never consults them: without this
                // fold a child could exceed its parent on exactly those axes.
                // Same conditions as this executor's own folds in 2/2a.
                if (checked && !inside) {
                    PermissionMode ceiling_ood = policy_ceiling_->out_of_workdir;
                    std::string ceiling_ood_why = dimension("out_of_workdir", ceiling_ood);
                    if (!policy_ceiling_->out_of_workdir_patterns.empty() && subjects.whole &&
                        subject_is_path) {
                        std::optional<PermissionMode> folded;
                        for (const auto& rule : policy_ceiling_->out_of_workdir_patterns) {
                            if (GlobMatch(rule.glob, *subjects.whole,
                                          /*slash_terminates=*/true) &&
                                (!folded || rule.mode < *folded)) {
                                folded = rule.mode;
                                ceiling_ood_why = "pattern `" + rule.glob + "`=" +
                                                  std::string(mode_name(rule.mode));
                            }
                        }
                        if (folded) ceiling_ood = *folded;
                    }
                    tighten(ceiling_ood, [&] {
                        return "parent session, outside the project: " + ceiling_ood_why;
                    });
                }
                if (tool.network_access_required()) {
                    tighten(policy_ceiling_->network, [&] {
                        return "parent session, network access: " +
                               dimension("network", policy_ceiling_->network);
                    });
                }
            }
        } else if (mcp_ceiling_) {
            // McpPermissionPolicy carries no out_of_workdir/network dimensions
            // to fold -- per-server modes (refined by the parent's per-tool
            // rules) are all it has.
            std::string ceiling_why;
            const PermissionMode ceiling_mode =
                policy_ceiling_ ? resolve_mcp_tool_mode(*policy_ceiling_, *mcp_ceiling_,
                                                        mcp_server, permission_key, &ceiling_why)
                                : mcp_ceiling_->mode_for(mcp_server);
            tighten(ceiling_mode, [&] {
                return "parent session: " +
                       (ceiling_why.empty() ? dimension("mcp " + mcp_server, ceiling_mode)
                                            : ceiling_why);
            });
        }
    }

    // 2c. The tool's own per-invocation narrowing (the skill tool marks ask-mode
    //     skills): the same most_restrictive fold as every dimension above, and
    //     deliberately BEFORE the run mode -- Bypass washes the narrowed Ask
    //     back to allow and AllowOnly clamps it to deny, exactly like any other
    //     Ask. A tool can tighten its own call, never widen one.
    if (mode != PermissionMode::AlwaysDeny) {
        if (auto narrowed = tool.mode_override_for(input, ctx)) {
            tighten(*narrowed, [&] {
                return "the tool requires " + std::string(mode_name(*narrowed)) +
                       " for this input";
            });
        }
    }

    // An Ask an operator's `ask` PATTERN produced is a demand to be asked
    // about this invocation: session grants must not satisfy it (grants are
    // blanket per binary; the pattern is not). Only a pattern-carrying policy
    // computed subjects, so a pattern-free pair pays nothing here.
    Verdict verdict;
    verdict.ask_grantable =
        !policy_.ask_pattern_matches(permission_key, subjects, subject_is_path) &&
        !(policy_ceiling_ &&
          policy_ceiling_->ask_pattern_matches(permission_key, subjects, subject_is_path));
    verdict.confinement_checked = checked;
    verdict.inside_project_root = checked && inside;

    // 3. Run mode wraps whatever the policy decided.
    const PermissionRunMode run_mode = run_mode_.load(std::memory_order_relaxed);
    verdict.mode = apply_run_mode(mode, run_mode);
    if (verdict.mode != mode) {
        why = run_mode == PermissionRunMode::AllowOnly
                  ? "run mode allow-only turns ask into deny (would ask: " + why + ")"
                  : "run mode bypass approves prompts (would ask: " + why + ")";
    }
    verdict.reason = std::move(why);
    return verdict;
}

llm::ToolResultBlock ToolExecutor::Run(const std::string& tool_use_id,
                                        const std::string& tool_name,
                                        const nlohmann::json& input, ToolContext& ctx) {
    PreparedInvocation prepared = Prepare(tool_use_id, tool_name, input, ctx);
    return Execute(prepared, ctx);
}

PreparedInvocation ToolExecutor::Prepare(const std::string& tool_use_id,
                                         const std::string& tool_name,
                                         const nlohmann::json& input, ToolContext& ctx) {
    // EVERY EXIT PATH ANSWERS THE CALL, including a throw, and that is a
    // history-integrity requirement rather than politeness. The agent loop
    // persists the assistant message carrying the tool_use BEFORE running any
    // tool and appends the results AFTER all of them, so an exception escaping
    // here leaves an orphaned tool_use in history -- the exact state
    // snap_compaction_boundary exists to prevent, and a hard 400 on both wire
    // formats. The session survives the throw (Session::RunOneTurn catches it)
    // and then fails every subsequent turn forever.
    //
    // A throw is expected here, not exceptional: everything below Find() is
    // plugin code -- category_for, Disclose and execute are all arbitrary native
    // code by design -- reading a JSON input the MODEL wrote. `{"offset": "1"}`
    // is a type_error out of nlohmann, and the model has no schema obligation it
    // can be held to. Every other plugin entry point in the tree is already
    // wrapped this way (observers, commands); this was the one that wasn't.
    try {
        return PrepareGuarded(tool_use_id, tool_name, input, ctx);
    } catch (const std::exception& error) {
        ctx.logger.warn("tool '{}' threw while gating: {}", tool_name, error.what());
        PreparedInvocation failed;
        failed.tool_use_id = tool_use_id;
        failed.tool_name = tool_name;
        failed.input = input;
        failed.denial = ErrorResult(tool_use_id,
                                    "tool '" + tool_name + "' failed: " + error.what());
        return failed;
    } catch (...) {
        ctx.logger.warn("tool '{}' threw a non-standard exception while gating", tool_name);
        PreparedInvocation failed;
        failed.tool_use_id = tool_use_id;
        failed.tool_name = tool_name;
        failed.input = input;
        failed.denial = ErrorResult(
            tool_use_id, "tool '" + tool_name + "' failed with an unrecognized exception");
        return failed;
    }
}

PreparedInvocation ToolExecutor::PrepareGuarded(const std::string& tool_use_id,
                                                const std::string& tool_name,
                                                const nlohmann::json& input, ToolContext& ctx) {
    PreparedInvocation prepared;
    prepared.tool_use_id = tool_use_id;
    prepared.tool_name = tool_name;
    prepared.input = input;

    // The shared_ptr copy is the lifetime guarantee: it outlives this call
    // even if a concurrent MCP replacement removes the tool from the registry
    // mid-invocation (the permission Ask below can block for minutes).
    std::shared_ptr<Tool> tool = registry_.Find(tool_name);
    if (tool == nullptr) {
        prepared.denial = ErrorResult(tool_use_id, "unknown tool: " + tool_name);
        return prepared;
    }
    prepared.tool = tool;

    // The invocation id belongs to the context before anything runs: a tool
    // that ties follow-up work to its own call (subagent completion injection)
    // reads it from there.
    ctx.tool_use_id = tool_use_id;

    // The confinement verdict rides out on the prepared invocation so Execute
    // can re-check it: the two phases are separated by the rest of the round's
    // gating, including Ask prompts that can sit for minutes.
    const Verdict verdict = Explain(*tool, input, ctx);
    const PermissionMode mode = verdict.mode;
    const bool ask_grantable = verdict.ask_grantable;
    prepared.path_confinement_checked = verdict.confinement_checked;
    prepared.was_inside_project_root = verdict.inside_project_root;

    if (mode == PermissionMode::AlwaysDeny) {
        // The rule rides along: the model reads WHY (a plan preset's
        // file_write=deny is not worth retrying; a missing grant under
        // allow-only is the operator's to change), and so does anyone
        // reading the transcript.
        prepared.denial = ErrorResult(
            tool_use_id, "permission denied for tool: " + tool_name + " (" + verdict.reason + ")");
        return prepared;
    }

    // --- core.pre_tool_use: the hook veto seam --------------------------------
    //
    // Placed AFTER the AlwaysDeny short-circuit -- a hook never sees a call the
    // policy already denied; hooks only narrow -- and BEFORE grants/Ask: a hook
    // deny suppresses the prompt entirely, which is the point (a guard that
    // denies must not still put a modal in front of the operator).
    //
    // HOOKS ONLY NARROW: everything above this line (mode lattice, precedence
    // chains, out-of-workdir fold, subagent ceilings, run mode) is final. A
    // hook can deny what policy allowed; it can never allow what policy
    // denied, because no path from here can raise `mode`.
    if (hooks_ != nullptr) {
        auto labels = hook_labels(tool_name, input);
        nlohmann::json payload{{"session_id", ctx.session_id},
                               {"tool_use_id", tool_use_id},
                               {"tool_name", tool_name},
                               {"permission_key", std::move(labels.permission_key)},
                               {"category", std::move(labels.category)},
                               {"tool_input", input}};
        if (auto blocked = hooks_->Emit(hooks::kPreToolUse, std::move(payload),
                                        ctx.is_cancelled)) {
            ctx.logger.warn("session {}: pre_tool_use hook blocked '{}': {}", ctx.session_id,
                            tool_name, *blocked);
            prepared.denial = ErrorResult(tool_use_id, "blocked by hook: " + *blocked);
            return prepared;
        }
    }

    if (mode == PermissionMode::Ask) {
        const PermissionDisclosure disclosure = tool->Disclose(input, ctx);
        if (!ask_grantable || !SatisfiedByGrants(disclosure)) {
            // core.permission_prompted: observe-only, fired the moment a human
            // is about to be asked. The natural feed for a desktop
            // notification -- "the agent is waiting on you" -- which is the
            // consumer story for it.
            if (hooks_ != nullptr) {
                auto labels = hook_labels(tool_name, input);
                nlohmann::json payload{{"session_id", ctx.session_id},
                                       {"tool_name", tool_name},
                                       {"permission_key", std::move(labels.permission_key)},
                                       {"category", std::move(labels.category)},
                                       {"summary", disclosure.summary}};
                hooks_->EmitAsync(hooks::kPermissionPrompted, std::move(payload));
            }
            PermissionRequest request;
            request.session_id = ctx.session_id;
            request.tool_name = tool_name;
            request.category = tool->category_for(input).value_or(tool->spec().category);
            request.tool_input = input;
            request.summary = disclosure.summary;
            request.reason = verdict.reason;
            // The prompt offers only what the operator does NOT already hold:
            // a compound command sharing binaries with an earlier one
            // (`cd x; ls a | wc -l`, then `cd x; ls a | grep something`)
            // re-listed every already-granted binary on every prompt --
            // prompt-fatigue noise, since a held key is an idempotent insert.
            // The conjunction above keeps running on the UNFILTERED
            // disclosure, so the prompt/no-prompt decision is unchanged; and
            // two words resolving to the same binary (`ls` and `/bin/ls` in
            // one command) dedupe to the first offer -- they mint the same
            // key anyway. The granted-subset-of-offered filter downstream
            // still bounds the answer, now to what THIS prompt showed.
            PermissionDisclosure shown = disclosure;
            shown.grants.clear();
            std::unordered_set<std::string> shown_keys;
            for (const auto& offer : disclosure.grants) {
                if (session_grants_.count(offer.key) != 0) continue;
                if (!shown_keys.insert(offer.key).second) continue;
                shown.grants.push_back(offer);
            }
            request.disclosure = std::move(shown);

            const PermissionResponse response = checker_.check(request);
            if (response.decision == PermissionDecision::Deny) {
                // The operator's message is the whole point of "deny with
                // message": the model reads WHY, not just that it was refused.
                std::string text = "permission denied by user for tool: " + tool_name;
                if (!response.deny_message.empty()) {
                    text += "\nmessage from the user: " + response.deny_message;
                }
                if (response.stop_turn) text += "\nthe user stopped the turn here";
                prepared.denial = ErrorResult(tool_use_id, text);
                prepared.stop_turn = response.stop_turn;
                return prepared;
            }
            // Only keys this invocation actually disclosed may be remembered: a
            // checker must not be able to grant something the operator was never
            // shown. This is the AUTHORITY for the rule -- it runs for every
            // checker kind, wire responses (already filtered at the boundary,
            // see proto_convert.hpp) and plugin advisors alike -- and is why a
            // hostile client or a runaway advisor can mint nothing.
            for (const auto& key : response.granted_keys) {
                const bool disclosed = std::any_of(
                    disclosure.grants.begin(), disclosure.grants.end(),
                    [&key](const GrantOffer& offer) { return offer.key == key; });
                if (disclosed) session_grants_.insert(key);
            }
        }
    }

    return prepared;
}

llm::ToolResultBlock ToolExecutor::Execute(const PreparedInvocation& prepared,
                                           ToolContext& ctx) {
    if (prepared.denial) {
        return *prepared.denial;
    }

    // Checked here rather than in Prepare: a cancellation that lands between
    // the gating phase and this call still denies the call, which matters now
    // that the two phases can be separated by the rest of the round's gating.
    if (ctx.is_cancelled && ctx.is_cancelled()) {
        return ErrorResult(prepared.tool_use_id,
                           "cancelled before execution: " + prepared.tool_name);
    }

    // TOCTOU re-check. The confinement classification in Prepare is not a lock:
    // between the two phases -- possibly minutes, if an Ask sat on a human --
    // the model can rename a symlink onto the path with a parallel bash call
    // and route the operation outside project_root while the approved verdict
    // said inside. Re-run the same confine() and refuse an inside->outside
    // DIVERGENCE. A Prepare-time OUTSIDE verdict is not a divergence: the
    // out-of-workdir fold already priced it in (possibly allowing it), so
    // second-guessing it here would deny what the operator explicitly allowed.
    // An unresolvable path fails closed, but only against a recorded inside:
    // a recorded outside (or an unchecked call) is the executor's own
    // classification and stays final.
    if (prepared.path_confinement_checked && prepared.tool->path_confinement_supported()) {
        if (auto path = prepared.tool->ExtractPath(prepared.input)) {
            const ConfineResult resolved = confine(ctx.project_root, *path, /*must_exist=*/false);
            if (prepared.was_inside_project_root &&
                (!resolved.ok || resolved.outside_project_root)) {
                return ErrorResult(prepared.tool_use_id,
                                   "path escaped the workspace between approval and execution: " +
                                       *path);
            }
        }
    }

    // Same never-throws contract as Prepare, same reasoning: execute() is
    // plugin code reading model-written input, and the call is already
    // persisted. The catch moved here from the old Run() so a parallel caller
    // gets the guarantee without composing through Run().
    try {
        ToolResult result = prepared.tool->execute(prepared.input, ctx);

        llm::ToolResultBlock block;
        block.tool_use_id = prepared.tool_use_id;
        block.is_error = result.is_error;
        // Never emit an empty text block: `touch x` succeeds silently, and an empty
        // block is a hard 400 on the Anthropic wire format. Systemic here rather than
        // per-tool, since any tool can produce no output.
        if (result.content_text.empty()) result.content_text = "(no output)";
        block.content.push_back(llm::TextBlock{std::move(result.content_text)});
        block.display = std::move(result.display);
        return block;
    } catch (const std::exception& error) {
        ctx.logger.warn("tool '{}' threw: {}", prepared.tool_name, error.what());
        return ErrorResult(prepared.tool_use_id,
                           "tool '" + prepared.tool_name + "' failed: " + error.what());
    } catch (...) {
        ctx.logger.warn("tool '{}' threw a non-standard exception", prepared.tool_name);
        return ErrorResult(prepared.tool_use_id,
                           "tool '" + prepared.tool_name +
                               "' failed with an unrecognized exception");
    }
}

std::optional<ToolExecutor::Verdict> ToolExecutor::Explain(const std::string& tool_name,
                                                          const nlohmann::json& input,
                                                          const ToolContext& ctx,
                                                          bool* grants_satisfy) const {
    const std::shared_ptr<Tool> tool = registry_.Find(tool_name);
    if (tool == nullptr) return std::nullopt;
    Verdict verdict = Explain(*tool, input, ctx);
    if (grants_satisfy != nullptr) {
        *grants_satisfy = verdict.mode == PermissionMode::Ask && verdict.ask_grantable &&
                          SatisfiedByGrants(tool->Disclose(input, ctx));
    }
    return verdict;
}

ToolExecutor::HookLabels ToolExecutor::hook_labels(const std::string& tool_name,
                                                   const nlohmann::json& input) const {
    HookLabels labels;
    labels.permission_key = std::string(registry_.PermissionKeyFor(tool_name));
    const std::shared_ptr<Tool> tool = registry_.Find(tool_name);
    if (tool == nullptr) {
        labels.category = mcp_server_of(tool_name).empty() ? "unknown" : "mcp";
        return labels;
    }
    try {
        labels.category =
            HookCategoryName(tool_name, tool->category_for(input).value_or(tool->spec().category));
    } catch (...) {
        // category_for is plugin code reading model-written input; a label
        // is never worth a throw out of a hook emission.
        labels.category = HookCategoryName(tool_name, tool->spec().category);
    }
    return labels;
}

bool ToolExecutor::SatisfiedByGrants(const PermissionDisclosure& disclosure) const {
    // An incomplete or empty disclosure never satisfies the conjunction. This is
    // a logic requirement, not a judgement about which programs are dangerous:
    // "nothing to check" must not evaluate to "check passed", or an invocation we
    // could not name would authorise itself while holding no grants at all.
    //
    // Note what this deliberately does NOT do: it does not second-guess a grant.
    // If the operator granted /bin/bash, `bash -c '...'` runs unprompted. Refusing
    // that while honouring a grant on git would be an interpreter denylist --
    // the same theatre as an executable allowlist, defeated the same way.
    if (!disclosure.complete || disclosure.grants.empty()) return false;

    for (const auto& offer : disclosure.grants) {
        if (session_grants_.find(offer.key) == session_grants_.end()) return false;
    }
    return true;
}

std::vector<std::string> ToolExecutor::granted_keys() const {
    std::vector<std::string> keys(session_grants_.begin(), session_grants_.end());
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace minimal::tools
