#include "minimal/tools/tool_executor.hpp"

#include <algorithm>
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

}  // namespace

PermissionMode ToolExecutor::ResolveMode(const Tool& tool, const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    return ResolveMode(tool, input, ctx, /*confinement_checked=*/nullptr,
                       /*was_inside_project_root=*/nullptr);
}

PermissionMode ToolExecutor::ResolveMode(const Tool& tool, const nlohmann::json& input,
                                          const ToolContext& ctx, bool* confinement_checked,
                                          bool* was_inside_project_root) const {
    const std::string& name = tool.spec().name;  // surface name: what the model calls it
    const ToolCategory category = tool.category_for(input).value_or(tool.spec().category);

    // 1. Is this tool allowed at all? MCP tools are server-scoped, not per-tool.
    //    The policy lookup uses the PERMISSION key (the non-alias name), never
    //    the surface: per_tool_overrides must keep governing a tool when an
    //    alias changes what the model sees. For unaliased tools the two agree.
    const std::string_view permission_key = registry_.PermissionKeyFor(name);
    const std::string mcp_server = mcp_server_of(name);

    // The pattern-matching subject, computed only when the policy actually
    // carries pattern rules (the confine() call below is filesystem work; a
    // pattern-free policy must not pay it on every invocation). A canonical
    // path anchors patterns like "**/.env" at "/" for confinement tools; the
    // raw subject stands for everything else (bash's command line).
    std::optional<std::string> pattern_subject;
    bool subject_is_path = false;
    if (!policy_.pattern_overrides.empty()) {
        if (auto raw = tool.PatternSubject(input)) {
            if (tool.path_confinement_supported()) {
                const ConfineResult canonical = confine(ctx.project_root, *raw, false);
                pattern_subject = canonical.ok ? canonical.absolute_path.string() : *raw;
                subject_is_path = true;
            } else {
                pattern_subject = std::move(*raw);
            }
        }
    }

    PermissionMode mode =
        mcp_server.empty()
            ? policy_.mode_for(permission_key, category, pattern_subject, subject_is_path)
            : mcp_policy_.mode_for(mcp_server);

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
        if (auto path = tool.ExtractPath(input)) {
            const ConfineResult resolved = confine(ctx.project_root, *path, /*must_exist=*/false);
            checked = true;
            inside = resolved.ok && !resolved.outside_project_root;
            if (!inside) {
                // An unresolvable path is treated as outside: fail closed rather
                // than silently applying the in-workdir rule to something we
                // could not place.
                PermissionMode ood = policy_.out_of_workdir;
                if (!policy_.out_of_workdir_patterns.empty() && resolved.ok) {
                    std::optional<PermissionMode> folded;
                    for (const auto& rule : policy_.out_of_workdir_patterns) {
                        if (GlobMatch(rule.glob, resolved.absolute_path.string(),
                                      /*slash_terminates=*/true)) {
                            folded = folded ? most_restrictive(*folded, rule.mode) : rule.mode;
                        }
                    }
                    if (folded) ood = *folded;
                }
                mode = most_restrictive(mode, ood);
            }
        }
    }

    // 2a. Same fold, same reason, for the network dimension: only if still
    //     permitted, and only for tools that declare network_access_required().
    if (mode != PermissionMode::AlwaysDeny && tool.network_access_required()) {
        mode = most_restrictive(mode, policy_.network);
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
                mode = most_restrictive(
                    mode, policy_ceiling_->mode_for(permission_key, category, pattern_subject,
                                                    subject_is_path));
                // out_of_workdir and network are dimension FIELDS, not
                // categories, so mode_for() never consults them: without this
                // fold a child could exceed its parent on exactly those axes.
                // Same conditions as this executor's own folds in 2/2a.
                if (checked && !inside) {
                    PermissionMode ceiling_ood = policy_ceiling_->out_of_workdir;
                    if (!policy_ceiling_->out_of_workdir_patterns.empty() && pattern_subject &&
                        subject_is_path) {
                        std::optional<PermissionMode> folded;
                        for (const auto& rule : policy_ceiling_->out_of_workdir_patterns) {
                            if (GlobMatch(rule.glob, *pattern_subject,
                                          /*slash_terminates=*/true)) {
                                folded =
                                    folded ? most_restrictive(*folded, rule.mode) : rule.mode;
                            }
                        }
                        if (folded) ceiling_ood = *folded;
                    }
                    mode = most_restrictive(mode, ceiling_ood);
                }
                if (tool.network_access_required()) {
                    mode = most_restrictive(mode, policy_ceiling_->network);
                }
            }
        } else if (mcp_ceiling_) {
            // McpPermissionPolicy carries no out_of_workdir/network dimensions
            // to fold -- per-server modes are all it has.
            mode = most_restrictive(mode, mcp_ceiling_->mode_for(mcp_server));
        }
    }

    // 2c. The tool's own per-invocation narrowing (the skill tool marks ask-mode
    //     skills): the same most_restrictive fold as every dimension above, and
    //     deliberately BEFORE the run mode -- Bypass washes the narrowed Ask
    //     back to allow and AllowOnly clamps it to deny, exactly like any other
    //     Ask. A tool can tighten its own call, never widen one.
    if (mode != PermissionMode::AlwaysDeny) {
        if (auto narrowed = tool.mode_override_for(input, ctx)) {
            mode = most_restrictive(mode, *narrowed);
        }
    }

    // 3. Run mode wraps whatever the policy decided.
    if (confinement_checked != nullptr) *confinement_checked = checked;
    if (was_inside_project_root != nullptr) *was_inside_project_root = checked && inside;
    return apply_run_mode(mode, run_mode_.load(std::memory_order_relaxed));
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
    const PermissionMode mode = ResolveMode(*tool, input, ctx,
                                            &prepared.path_confinement_checked,
                                            &prepared.was_inside_project_root);

    if (mode == PermissionMode::AlwaysDeny) {
        prepared.denial = ErrorResult(tool_use_id, "permission denied for tool: " + tool_name);
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
        const std::string permission_key(registry_.PermissionKeyFor(tool_name));
        const ToolCategory category =
            tool->category_for(input).value_or(tool->spec().category);
        nlohmann::json payload{{"session_id", ctx.session_id},
                               {"tool_use_id", tool_use_id},
                               {"tool_name", tool_name},
                               {"permission_key", permission_key},
                               {"category", CategoryName(category)},
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
        if (!SatisfiedByGrants(disclosure)) {
            // core.permission_prompted: observe-only, fired the moment a human
            // is about to be asked. The natural feed for a desktop
            // notification -- "the agent is waiting on you" -- which is the
            // consumer story for it.
            if (hooks_ != nullptr) {
                const std::string permission_key(registry_.PermissionKeyFor(tool_name));
                nlohmann::json payload{{"session_id", ctx.session_id},
                                       {"tool_name", tool_name},
                                       {"permission_key", permission_key},
                                       {"category",
                                        CategoryName(tool->category_for(input)
                                                         .value_or(tool->spec().category))},
                                       {"summary", disclosure.summary}};
                hooks_->EmitAsync(hooks::kPermissionPrompted, std::move(payload));
            }
            PermissionRequest request;
            request.session_id = ctx.session_id;
            request.tool_name = tool_name;
            request.category = tool->category_for(input).value_or(tool->spec().category);
            request.tool_input = input;
            request.summary = disclosure.summary;
            request.disclosure = disclosure;

            const PermissionResponse response = checker_.check(request);
            if (response.decision == PermissionDecision::Deny) {
                prepared.denial =
                    ErrorResult(tool_use_id, "permission denied by user for tool: " + tool_name);
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
