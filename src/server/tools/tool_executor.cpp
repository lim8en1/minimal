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
                            std::vector<std::string> seed_grants)
    : registry_(registry),
      checker_(checker),
      policy_(std::move(policy)),
      mcp_policy_(std::move(mcp_policy)),
      run_mode_(run_mode),
      policy_ceiling_(std::move(policy_ceiling)),
      mcp_ceiling_(std::move(mcp_ceiling)),
      session_grants_(seed_grants.begin(), seed_grants.end()) {}

PermissionMode ToolExecutor::ResolveMode(const Tool& tool, const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    const std::string& name = tool.spec().name;
    const ToolCategory category = tool.category_for(input).value_or(tool.spec().category);

    // 1. Is this tool allowed at all? MCP tools are server-scoped, not per-tool.
    const std::string mcp_server = mcp_server_of(name);
    PermissionMode mode = mcp_server.empty() ? policy_.mode_for(name, category)
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
    if (mode != PermissionMode::AlwaysDeny) {
        if (mcp_server.empty()) {
            if (policy_ceiling_) mode = most_restrictive(mode, policy_ceiling_->mode_for(name, category));
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
    Tool* tool = registry_.Find(tool_name);
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

    if (mode == PermissionMode::Ask) {
        const PermissionDisclosure disclosure = tool->Disclose(input, ctx);
        if (!SatisfiedByGrants(disclosure)) {
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
