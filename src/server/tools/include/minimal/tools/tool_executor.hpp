#pragma once

#include <string>
#include <unordered_set>

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

    // True when every key this invocation discloses has already been granted, so
    // the prompt can be skipped. Exposed for testing the invariants directly.
    bool SatisfiedByGrants(const PermissionDisclosure& disclosure) const;

    std::size_t granted_count() const { return session_grants_.size(); }

    // /permission-mode. Worker-thread only: changing it is not a read-only
    // command, so it runs between turns rather than beside one -- which is what
    // makes this safe without a lock while a tool dispatch reads it.
    void SetRunMode(PermissionRunMode mode) { run_mode_ = mode; }
    PermissionRunMode run_mode() const { return run_mode_; }

private:
    ToolRegistry& registry_;
    PermissionChecker& checker_;
    PermissionPolicy policy_;
    McpPermissionPolicy mcp_policy_;
    PermissionRunMode run_mode_;

    // Session-scoped and in-memory, deliberately: never written to config or to
    // the session store. A grant's value is a human judging a specific disclosed
    // invocation at that moment; persisting it keeps the shape and discards the
    // substance, which is how a consent affordance decays into an allowlist.
    // ToolExecutor already holds a per-session PermissionPolicy by value, so it
    // is the natural owner.
    std::unordered_set<std::string> session_grants_;
};

// Extracts "<server>" from "mcp__<server>__<tool>". Returns empty for a name
// that is not MCP-namespaced.
std::string mcp_server_of(const std::string& tool_name);

}  // namespace minimal::tools
