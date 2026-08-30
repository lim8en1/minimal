#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

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
    // The optional ceilings exist for subagents: a child session may never be
    // more permissive than its parent, but a child preset is resolved from
    // config with no knowledge of who spawns it. The ceiling is the parent's
    // policy, handed down at spawn time and folded in via most_restrictive()
    // AFTER this executor's own resolution and BEFORE apply_run_mode() -- so a
    // per-tool override in the child preset cannot buy back what the parent's
    // corresponding rule forbids. Folding here rather than merging policies up
    // front is what keeps per-tool overrides correct: mode_for() knows the
    // tool's category, and a merged policy no longer does.
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker, PermissionPolicy policy,
                 McpPermissionPolicy mcp_policy, PermissionRunMode run_mode,
                 std::optional<PermissionPolicy> policy_ceiling = std::nullopt,
                 std::optional<McpPermissionPolicy> mcp_ceiling = std::nullopt,
                 std::vector<std::string> seed_grants = {});

    llm::ToolResultBlock Run(const std::string& tool_use_id, const std::string& tool_name,
                              const nlohmann::json& input, ToolContext& ctx);

    // Exposed for testing and for surfacing "what would happen" without running.
    PermissionMode ResolveMode(const Tool& tool, const nlohmann::json& input,
                                const ToolContext& ctx) const;

    // True when every key this invocation discloses has already been granted, so
    // the prompt can be skipped. Exposed for testing the invariants directly.
    bool SatisfiedByGrants(const PermissionDisclosure& disclosure) const;

    std::size_t granted_count() const { return session_grants_.size(); }

    // Session grants are copied into a subagent at spawn -- re-prompting for
    // every binary the parent already approved would make subagents unusable --
    // so the spawning side needs to read them. Sorted for deterministic tests.
    std::vector<std::string> granted_keys() const;

    // The parent's run mode is the ceiling a child's is clamped against
    // (clamp_run_mode), which a spawner cannot do without reading it.
    PermissionRunMode run_mode() const { return run_mode_; }

    // The policies a spawner hands DOWN as its child's ceilings -- the
    // per-category half of "a child is never more permissive than its parent".
    const PermissionPolicy& policy() const { return policy_; }
    const McpPermissionPolicy& mcp_policy() const { return mcp_policy_; }

private:
    ToolRegistry& registry_;
    PermissionChecker& checker_;
    PermissionPolicy policy_;
    McpPermissionPolicy mcp_policy_;
    PermissionRunMode run_mode_;
    std::optional<PermissionPolicy> policy_ceiling_;
    std::optional<McpPermissionPolicy> mcp_ceiling_;

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
