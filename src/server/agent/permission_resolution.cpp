#include "minimal/agent/permission_resolution.hpp"

#include <algorithm>
#include <memory>

namespace minimal::agent {

namespace {
// Never a real tool name (permissions.json and agent-preset-frontmatter both
// reject an empty-string per_tool_overrides key), so probing with it yields
// the category-level summary with no per-tool rule participating.
constexpr const char* kCategoryProbeToolName = "";
}  // namespace

tools::PermissionPolicy BuildPermissionPolicy(
    const tools::PermissionOverrideLayer& preset, const tools::PermissionOverrideLayer& global,
    bool preset_is_trusted, const std::vector<std::string>& dotenv_protected_permission_keys) {
    tools::PermissionPolicy policy;

    // Tool/category resolution runs per call, from the layers themselves --
    // the tool's category is only known then (see PermissionPolicy::layers).
    // Patterns and the dimension scalars below are merged here as before.
    tools::ToolModeLayers layers{preset, global, preset_is_trusted};
    layers.preset.pattern_overrides.clear();  // merged into the flat set below
    layers.global.pattern_overrides.clear();
    layers.preset.out_of_workdir_patterns.clear();
    layers.global.out_of_workdir_patterns.clear();
    policy.layers = std::make_shared<const tools::ToolModeLayers>(std::move(layers));

    // Category-level summary (no tool named): what the flat fields report to
    // anything inspecting the policy. kCategoryProbeToolName is never a real
    // key, so no per-tool rule participates.
    policy.read_only = policy.mode_for(kCategoryProbeToolName, tools::ToolCategory::ReadOnly);
    policy.file_write = policy.mode_for(kCategoryProbeToolName, tools::ToolCategory::FileWrite);
    policy.execute = policy.mode_for(kCategoryProbeToolName, tools::ToolCategory::Execute);

    // Pattern precedence: preset -> permissions.json -> Restrictive. An
    // untrusted preset cannot choose Specific (it can widen).
    policy.pattern_precedence =
        (preset_is_trusted ? preset.pattern_precedence : std::nullopt)
            .value_or(global.pattern_precedence.value_or(tools::PatternPrecedence::Restrictive));

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

    // Summary only: resolution reads policy.layers.
    policy.per_tool_overrides = global.per_tool_overrides;
    for (const auto& [name, mode] : preset.per_tool_overrides) {
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
    // The built-in defaults (dotenv denial for tools that opt in) merge per
    // (tool, glob), the same rule merge_pattern_layers follows: a config rule
    // on the SAME glob replaces the default (that is what keeps it a default,
    // not a floor -- `"**/.env": allow` lifts it), and every other default
    // glob stays. Replacing them wholesale per tool meant any unrelated rule
    // (`write: {"**/plans/**": allow}`) silently re-opened .env for that tool.
    for (auto& [key, rules] : tools::default_pattern_rules(dotenv_protected_permission_keys)) {
        auto& out = policy.pattern_overrides[key];
        for (auto& rule : rules) {
            const bool spoken = std::any_of(out.begin(), out.end(), [&](const auto& existing) {
                return existing.glob == rule.glob;
            });
            if (!spoken) out.push_back(std::move(rule));
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
