#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "minimal/llm/message.hpp"
#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::tools {

// The single call site the agent loop uses for tool invocation. Permission
// gating lives entirely here: Tool::execute() implementations never reference
// PermissionChecker, so the safety model cannot vary per tool.
class ToolExecutor {
public:
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker, PermissionPolicy policy,
                 McpPermissionPolicy mcp_policy, PermissionRunMode run_mode);

    llm::ToolResultBlock Run(const std::string& tool_use_id, const std::string& tool_name,
                              const nlohmann::json& input, ToolContext& ctx);

    // Exposed for testing and for surfacing "what would happen" without running.
    PermissionMode ResolveMode(const Tool& tool, const nlohmann::json& input,
                                const ToolContext& ctx) const;

private:
    ToolRegistry& registry_;
    PermissionChecker& checker_;
    PermissionPolicy policy_;
    McpPermissionPolicy mcp_policy_;
    PermissionRunMode run_mode_;
};

// Extracts "<server>" from "mcp__<server>__<tool>". Returns empty for a name
// that is not MCP-namespaced.
std::string mcp_server_of(const std::string& tool_name);

}  // namespace minimal::tools
