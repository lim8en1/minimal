#include "minimal/agent/permission_resolution.hpp"

namespace minimal::agent {

namespace {
// Never a real tool name (permissions.json and agent-preset-frontmatter both
// reject an empty-string per_tool_overrides key), so probing with it forces
// resolve_permission_chain() to skip straight to category-level resolution.
constexpr const char* kCategoryProbeToolName = "";
}  // namespace

tools::PermissionPolicy BuildPermissionPolicy(
    const tools::PermissionOverrideLayer& preset, const tools::PermissionOverrideLayer& global,
    bool preset_is_trusted, const std::vector<std::string>& dotenv_protected_permission_keys) {
    tools::PermissionPolicy policy;

    // Untrusted mode: fold the preset-applied resolution against what global
    // ALONE would have produced, taking the more restrictive of the two --
    // the preset can narrow (e.g. Allow -> Ask) but never widen (Ask -> Allow).
    auto category = [&](tools::ToolCategory cat) {
        const auto with_preset =
            tools::resolve_permission_chain(kCategoryProbeToolName, cat, preset, global);
        if (preset_is_trusted) return with_preset;
        const auto global_only = tools::resolve_permission_chain(
            kCategoryProbeToolName, cat, tools::PermissionOverrideLayer{}, global);
        return tools::most_restrictive(with_preset, global_only);
    };
    policy.read_only = category(tools::ToolCategory::ReadOnly);
    policy.file_write = category(tools::ToolCategory::FileWrite);
    policy.execute = category(tools::ToolCategory::Execute);

    policy.out_of_workdir = tools::resolve_out_of_workdir_chain(preset, global);
    policy.network = tools::resolve_network_chain(preset, global);
    if (!preset_is_trusted) {
        policy.out_of_workdir = tools::most_restrictive(
            policy.out_of_workdir,
            tools::resolve_out_of_workdir_chain(tools::PermissionOverrideLayer{}, global));
        policy.network = tools::most_restrictive(
            policy.network,
            tools::resolve_network_chain(tools::PermissionOverrideLayer{}, global));
    }

    policy.per_tool_overrides = global.per_tool_overrides;  // union...
    for (const auto& [name, mode] : preset.per_tool_overrides) {
        // ...preset wins on conflict -- but an UNTRUSTED preset may only ever
        // narrow via AlwaysDeny. Ask/AlwaysAllow from an untrusted per-tool
        // override is dropped rather than applied: confirming it would still
        // be a narrowing for an arbitrary tool name requires that tool's
        // ToolCategory, which this function does not have.
        if (preset_is_trusted || mode == tools::PermissionMode::AlwaysDeny) {
            policy.per_tool_overrides[name] = mode;
        }
    }

    // Pattern rules: an UNTRUSTED preset contributes NONE. Unlike a scalar
    // mode, a pattern's effect depends on what it matches at resolve time, so
    // "it only narrowed" cannot be established here -- a repo-shipped
    // `{"git *": "allow"}` would be a widening the clamp never sees. Trusted
    // presets merge over the global layer per (tool, glob).
    if (preset_is_trusted) {
        tools::merge_pattern_layers(preset, global, policy.pattern_overrides,
                                    policy.out_of_workdir_patterns);
    } else {
        tools::merge_pattern_layers(tools::PermissionOverrideLayer{}, global,
                                    policy.pattern_overrides, policy.out_of_workdir_patterns);
    }
    // The built-in defaults (dotenv denial for tools that opt in) fill in only
    // where neither layer spoke: operator patterns replace them wholesale per
    // tool, which is what makes them a default rather than a floor.
    for (auto& [key, rules] : tools::default_pattern_rules(dotenv_protected_permission_keys)) {
        if (policy.pattern_overrides.find(key) == policy.pattern_overrides.end()) {
            policy.pattern_overrides.emplace(key, std::move(rules));
        }
    }
    return policy;
}

tools::McpPermissionPolicy BuildMcpPermissionPolicy(
    const std::unordered_map<std::string, tools::PermissionMode>& preset_mcp_permissions,
    tools::PermissionMode global_mcp_default_mode,
    bool preset_is_trusted) {
    tools::McpPermissionPolicy policy;
    // Untrusted mode: fold each per-server mode against what the global
    // default ALONE would produce -- the preset may narrow (Allow -> Ask, or
    // anything -> Deny) but never widen (Ask -> Allow). Same trust clamp as
    // the sibling BuildPermissionPolicy: a repo-shipped preset must not be
    // able to auto-approve MCP calls.
    for (const auto& [server, mode] : preset_mcp_permissions) {
        policy.per_server[server] = preset_is_trusted
                                        ? mode
                                        : tools::most_restrictive(mode, global_mcp_default_mode);
    }
    policy.default_mode = global_mcp_default_mode;
    return policy;
}

}  // namespace minimal::agent
