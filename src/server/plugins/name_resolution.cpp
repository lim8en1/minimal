#include "minimal/plugins/name_resolution.hpp"

#include <algorithm>

namespace minimal::plugins {

ToolNameResolution ResolveToolNames(const tools::ToolRegistry& registry,
                                    const std::map<std::string, std::string>& aliases) {
    NameRegistry name_registry;

    for (const auto& attribution : registry.attributions()) {
        name_registry.Register(NameKind::Tool, attribution.plugin_name, attribution.tool_name);
    }
    for (const auto& [qualified_key, alias] : aliases) {
        name_registry.SetAlias(NameKind::Tool, qualified_key, alias);
    }

    ToolNameResolution resolution;
    resolution.result = name_registry.Resolve();

    std::sort(resolution.result.fatal_collisions.begin(), resolution.result.fatal_collisions.end());
    std::sort(resolution.result.dangling_aliases.begin(), resolution.result.dangling_aliases.end());

    for (const auto& entry : resolution.result.entries) {
        resolution.permission_keys.emplace(entry.surface_name, entry.permission_key);
    }
    return resolution;
}

}  // namespace minimal::plugins
