#include "minimal/plugins/name_resolution.hpp"

#include <algorithm>
#include <unordered_set>

namespace minimal::plugins {
namespace {

std::string Qualify(const std::string& plugin_name, const std::string& original_name) {
    return plugin_name + "." + original_name;
}

}  // namespace

RegistrationNames ResolveRegistrationNames(const tools::ToolRegistry& registry,
                                           const std::map<std::string, std::string>& aliases,
                                           const commands::CommandRegistry* commands,
                                           const std::vector<std::string>& core_tool_names) {
    NameRegistry name_registry;

    // Stable identities per kind, so an alias can be routed to whichever kind
    // claims it. Without this a command alias would be reported dangling because
    // no TOOL answers to it, which is a diagnostic pointing at nothing.
    std::unordered_set<std::string> tool_identities;
    std::unordered_set<std::string> command_identities;

    for (const auto& attribution : registry.attributions()) {
        name_registry.Register(NameKind::Tool, attribution.plugin_name, attribution.tool_name);
        tool_identities.insert(Qualify(attribution.plugin_name, attribution.tool_name));
    }
    // Reserved core names register at serve stage, after this pass; feed them in
    // as the built-ins (empty plugin name) they are, so a plugin claiming one is
    // a detected collision rather than a silent shadow. See the header comment.
    for (const auto& name : core_tool_names) {
        name_registry.Register(NameKind::Tool, "", name);
        tool_identities.insert(Qualify("", name));
    }
    if (commands != nullptr) {
        for (const auto& attribution : commands->attributions()) {
            name_registry.Register(NameKind::Command, attribution.plugin_name,
                                   attribution.original_name);
            command_identities.insert(Qualify(attribution.plugin_name, attribution.original_name));
        }
    }

    for (const auto& [qualified_key, alias] : aliases) {
        if (command_identities.count(qualified_key) != 0) {
            name_registry.SetAlias(NameKind::Command, qualified_key, alias);
        } else {
            // Tool kind covers both "a tool claims it" and "nothing claims it" --
            // the latter is what makes NameRegistry report it dangling, and one
            // kind reporting it once is what keeps the diagnostic honest.
            name_registry.SetAlias(NameKind::Tool, qualified_key, alias);
        }
    }

    RegistrationNames resolution;
    resolution.result = name_registry.Resolve();

    std::sort(resolution.result.fatal_collisions.begin(), resolution.result.fatal_collisions.end());
    std::sort(resolution.result.dangling_aliases.begin(), resolution.result.dangling_aliases.end());

    // TOOLS ONLY. Resolve() merges both kinds into `entries` without a kind tag,
    // and commands have no permission keys -- so a command surface name landing
    // in this map would answer a permission question nobody asked.
    for (const auto& entry : resolution.result.entries) {
        if (tool_identities.count(entry.stable_identity) == 0) continue;
        resolution.permission_keys.emplace(entry.surface_name, entry.permission_key);
    }
    return resolution;
}

}  // namespace minimal::plugins
