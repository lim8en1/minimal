#include "minimal/tools/tool_executor.hpp"

#include <algorithm>
#include <utility>

#include "minimal/tools/path_confinement.hpp"

namespace minimal::tools {
namespace {

constexpr std::string_view kMcpPrefix = "mcp__";
constexpr std::string_view kMcpSeparator = "__";

llm::ToolResultBlock ErrorResult(const std::string& tool_use_id, std::string text) {
    llm::ToolResultBlock block;
    block.tool_use_id = tool_use_id;
    block.is_error = true;
    block.content.push_back(llm::TextBlock{std::move(text)});
    return block;
}

}  // namespace

std::string mcp_server_of(const std::string& tool_name) {
    if (tool_name.rfind(kMcpPrefix, 0) != 0) return {};
    const auto rest = tool_name.substr(kMcpPrefix.size());
    const auto sep = rest.find(kMcpSeparator);
    if (sep == std::string::npos) return {};
    return rest.substr(0, sep);
}

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
    const std::string& name = tool.spec().name;  // surface name: what the model calls it
    const ToolCategory category = tool.category_for(input).value_or(tool.spec().category);

    // 1. Is this tool allowed at all? MCP tools are server-scoped, not per-tool.
    //    The policy lookup uses the PERMISSION key (the non-alias name), never
    //    the surface: per_tool_overrides must keep governing a tool when an
    //    alias changes what the model sees. For unaliased tools the two agree.
    const std::string_view permission_key = registry_.PermissionKeyFor(name);
    const std::string mcp_server = mcp_server_of(name);
    PermissionMode mode =
        mcp_server.empty() ? policy_.mode_for(permission_key, category)
                           : mcp_policy_.mode_for(mcp_server);

    // 2. Only if permitted, and only for path-bearing tools, fold in
    //    out_of_workdir. This is a FOLD, never a substitution: substituting
    //    would mean `file_write: deny` + `out_of_workdir: allow` permits writing
    //    /etc/foo while denying ./foo.cpp -- exactly inverted.
    if (mode != PermissionMode::AlwaysDeny && tool.path_confinement_supported()) {
        if (auto path = tool.ExtractPath(input)) {
            const ConfineResult resolved = confine(ctx.project_root, *path, /*must_exist=*/false);
            if (!resolved.ok || resolved.outside_project_root) {
                // An unresolvable path is treated as outside: fail closed rather
                // than silently applying the in-workdir rule to something we
                // could not place.
                mode = most_restrictive(mode, policy_.out_of_workdir);
            }
        }
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
                mode = most_restrictive(mode, policy_ceiling_->mode_for(permission_key, category));
            }
        } else if (mcp_ceiling_) {
            mode = most_restrictive(mode, mcp_ceiling_->mode_for(mcp_server));
        }
    }

    // 3. Run mode wraps whatever the policy decided.
    return apply_run_mode(mode, run_mode_);
}

llm::ToolResultBlock ToolExecutor::Run(const std::string& tool_use_id,
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
        return RunGuarded(tool_use_id, tool_name, input, ctx);
    } catch (const std::exception& error) {
        ctx.logger.warn("tool '{}' threw: {}", tool_name, error.what());
        return ErrorResult(tool_use_id, "tool '" + tool_name + "' failed: " + error.what());
    } catch (...) {
        ctx.logger.warn("tool '{}' threw a non-standard exception", tool_name);
        return ErrorResult(tool_use_id,
                           "tool '" + tool_name + "' failed with an unrecognized exception");
    }
}

llm::ToolResultBlock ToolExecutor::RunGuarded(const std::string& tool_use_id,
                                               const std::string& tool_name,
                                               const nlohmann::json& input, ToolContext& ctx) {
    // The shared_ptr copy is the lifetime guarantee: it outlives this call
    // even if a concurrent MCP replacement removes the tool from the registry
    // mid-invocation (the permission Ask below can block for minutes).
    std::shared_ptr<Tool> tool = registry_.Find(tool_name);
    if (tool == nullptr) {
        return ErrorResult(tool_use_id, "unknown tool: " + tool_name);
    }

    // The invocation id belongs to the context before anything runs: a tool
    // that ties follow-up work to its own call (subagent completion injection)
    // reads it from there.
    ctx.tool_use_id = tool_use_id;

    const PermissionMode mode = ResolveMode(*tool, input, ctx);

    if (mode == PermissionMode::AlwaysDeny) {
        return ErrorResult(tool_use_id, "permission denied for tool: " + tool_name);
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
            return ErrorResult(tool_use_id, "blocked by hook: " + *blocked);
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
                return ErrorResult(tool_use_id, "permission denied by user for tool: " + tool_name);
            }
            // Only keys this invocation actually disclosed may be remembered: a
            // checker must not be able to grant something the operator was never
            // shown.
            for (const auto& key : response.granted_keys) {
                const bool disclosed = std::any_of(
                    disclosure.grants.begin(), disclosure.grants.end(),
                    [&key](const GrantOffer& offer) { return offer.key == key; });
                if (disclosed) session_grants_.insert(key);
            }
        }
    }

    if (ctx.is_cancelled && ctx.is_cancelled()) {
        return ErrorResult(tool_use_id, "cancelled before execution: " + tool_name);
    }

    ToolResult result = tool->execute(input, ctx);

    llm::ToolResultBlock block;
    block.tool_use_id = tool_use_id;
    block.is_error = result.is_error;
    // Never emit an empty text block: `touch x` succeeds silently, and an empty
    // block is a hard 400 on the Anthropic wire format. Systemic here rather than
    // per-tool, since any tool can produce no output.
    if (result.content_text.empty()) result.content_text = "(no output)";
    block.content.push_back(llm::TextBlock{std::move(result.content_text)});
    return block;
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
