#include "minimal/tools/tool_executor.hpp"

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
                            PermissionRunMode run_mode)
    : registry_(registry),
      checker_(checker),
      policy_(std::move(policy)),
      mcp_policy_(std::move(mcp_policy)),
      run_mode_(run_mode) {}

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

    const PermissionMode mode = ResolveMode(*tool, input, ctx);

    if (mode == PermissionMode::AlwaysDeny) {
        return ErrorResult(tool_use_id, "permission denied for tool: " + tool_name);
    }

    if (mode == PermissionMode::Ask) {
        PermissionRequest request;
        request.session_id = ctx.session_id;
        request.tool_name = tool_name;
        request.category = tool->category_for(input).value_or(tool->spec().category);
        request.tool_input = input;
        request.summary = tool_name;
        if (checker_.check(request) == PermissionDecision::Deny) {
            return ErrorResult(tool_use_id, "permission denied by user for tool: " + tool_name);
        }
    }

    if (ctx.is_cancelled && ctx.is_cancelled()) {
        return ErrorResult(tool_use_id, "cancelled before execution: " + tool_name);
    }

    ToolResult result = tool->execute(input, ctx);

    llm::ToolResultBlock block;
    block.tool_use_id = tool_use_id;
    block.is_error = result.is_error;
    block.content.push_back(llm::TextBlock{std::move(result.content_text)});
    return block;
}

}  // namespace minimal::tools
