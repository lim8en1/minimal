#include "minimal/plugins/name_resolution.hpp"

#include <algorithm>
#include <array>

namespace minimal::plugins {
namespace {

// The permission key for each resolved tool. Bare in two disjoint cases:
//  - no other registration ever claimed this original name -- there was never
//    any ambiguity, so aliasing a single tool for typing convenience must not
//    detach its existing per-tool rule; or
//  - this entry kept its original name as its surface AND no other unaliased
//    entry still claims that name -- the collision was resolved by moving
//    every other claimant away.
// An entry that itself moved away while another claimant of its original
// name remains keeps its stable identity: the two are distinct tools that
// once shared a name, and a rule keyed on the bare name must not govern both.
std::unordered_map<std::string, std::string> PermissionKeys(const NameTable::Result& tools) {
    std::unordered_map<std::string, int> total;
    std::unordered_map<std::string, int> unaliased;
    for (const auto& e : tools.entries) {
        ++total[e.original_name];
        if (!e.aliased) ++unaliased[e.original_name];
    }
    std::unordered_map<std::string, std::string> keys;
    for (const auto& e : tools.entries) {
        const bool never_contested = total[e.original_name] <= 1;
        const bool collision_resolved_here = !e.aliased && unaliased[e.original_name] <= 1;
        keys.emplace(e.surface_name,
                     never_contested || collision_resolved_here ? e.original_name
                                                                : e.stable_identity);
    }
    return keys;
}

}  // namespace

RegistrationNames ResolveRegistrationNames(const tools::ToolRegistry& registry,
                                           const std::map<std::string, std::string>& aliases,
                                           const commands::CommandRegistry* commands,
                                           const std::vector<std::string>& core_tool_names,
                                           const ServiceRegistry* services,
                                           const hooks::HookDispatcher* hook_points) {
    using Policy = NameTable::Policy;
    NameTable tools(Policy::kName);
    NameTable command_names(Policy::kName);
    NameTable service_names(Policy::kSlot);
    NameTable hook_point_names(Policy::kSlot);

    for (const auto& attribution : registry.attributions()) {
        tools.Register(attribution.plugin_name, attribution.tool_name);
    }
    // Reserved core names register at serve stage, after this pass; fed in as
    // the built-ins (empty plugin name) they are.
    for (const auto& name : core_tool_names) tools.Register("", name);
    if (commands != nullptr) {
        for (const auto& attribution : commands->attributions()) {
            command_names.Register(attribution.plugin_name, attribution.original_name);
        }
    }
    if (services != nullptr) {
        for (const auto& attribution : services->attributions()) {
            service_names.Register(attribution.plugin_name, attribution.service_name);
        }
    }
    // A hook point's name is already "plugin.point"; its identity is never
    // addressed (slots are not aliased), only its collisions matter.
    if (hook_points != nullptr) {
        for (const auto& attribution : hook_points->attributions()) {
            hook_point_names.Register(attribution.plugin_name, attribution.point_name);
        }
    }

    const std::array<std::pair<NameSpace, NameTable*>, 4> spaces{{
        {NameSpace::kTool, &tools},
        {NameSpace::kCommand, &command_names},
        {NameSpace::kService, &service_names},
        {NameSpace::kHookPoint, &hook_point_names},
    }};
    const std::array<NameTable*, 4> tables{&tools, &command_names, &service_names,
                                           &hook_point_names};

    RegistrationNames resolution;
    AliasRouting routing = RouteAliases(aliases, tables);
    resolution.dangling_aliases = std::move(routing.dangling);
    resolution.slot_aliases = std::move(routing.targets_slot);

    for (const auto& [space, table] : spaces) {
        NameTable::Result result = table->Resolve();
        for (auto& collision : result.collisions) {
            resolution.collisions.push_back(
                {space, std::move(collision.surface), std::move(collision.claimants)});
        }
        for (auto& identity : result.ambiguous_identities) {
            resolution.ambiguous_identities.push_back(std::move(identity));
        }
        if (space == NameSpace::kTool) resolution.permission_keys = PermissionKeys(result);
    }
    // `spaces` is in enum order and each table's collisions are sorted, so
    // collisions are already sorted by (space, surface).
    auto& ambiguous = resolution.ambiguous_identities;
    std::sort(ambiguous.begin(), ambiguous.end());
    ambiguous.erase(std::unique(ambiguous.begin(), ambiguous.end()), ambiguous.end());
    return resolution;
}

}  // namespace minimal::plugins
