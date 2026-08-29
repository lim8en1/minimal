#include "minimal/agent/permission_resolution.hpp"

namespace minimal::agent {

namespace {
// Never a real tool name (permissions.json and agent-preset-frontmatter both
// reject an empty-string per_tool_overrides key), so probing with it forces
// resolve_permission_chain() to skip straight to category-level resolution.
constexpr const char* kCategoryProbeToolName = "";
}  // namespace

tools::PermissionPolicy BuildPermissionPolicy(const tools::PermissionOverrideLayer& preset,
                                               const tools::PermissionOverrideLayer& global) {
    tools::PermissionPolicy policy;
    policy.read_only =
        tools::resolve_permission_chain(kCategoryProbeToolName, tools::ToolCategory::ReadOnly, preset, global);
    policy.file_write =
        tools::resolve_permission_chain(kCategoryProbeToolName, tools::ToolCategory::FileWrite, preset, global);
    policy.execute =
        tools::resolve_permission_chain(kCategoryProbeToolName, tools::ToolCategory::Execute, preset, global);
    policy.out_of_workdir = tools::resolve_out_of_workdir_chain(preset, global);

    policy.per_tool_overrides = global.per_tool_overrides;  // union...
    for (const auto& [name, mode] : preset.per_tool_overrides) {
        policy.per_tool_overrides[name] = mode;  // ...preset wins on conflict
    }
    return policy;
}

tools::McpPermissionPolicy BuildMcpPermissionPolicy(
    const std::unordered_map<std::string, tools::PermissionMode>& preset_mcp_permissions,
    tools::PermissionMode global_mcp_default_mode) {
    tools::McpPermissionPolicy policy;
    policy.per_server = preset_mcp_permissions;
    policy.default_mode = global_mcp_default_mode;
    return policy;
}

}  // namespace minimal::agent
