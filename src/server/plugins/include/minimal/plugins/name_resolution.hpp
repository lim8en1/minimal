#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/plugins/name_table.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

// The three-naming-levels rule (ARCHITECTURE.md, "Name collisions and aliases")
// applied to everything registered so far. The server's namespaces and their
// policy (common's NameTable is kind-agnostic):
//   tools, commands  -- names: aliasable via plugins.json `aliases`
//   services, hook points -- slots: addressed by code/config, never aliased
// Separate namespaces, so a tool and a command may share a surface name.
enum class NameSpace { kTool, kCommand, kService, kHookPoint };

struct NameCollision {
    NameSpace space;
    std::string surface;
    std::vector<std::string> claimants;  // plugin names, "" for core
};

struct RegistrationNames {
    std::vector<NameCollision> collisions;          // fatal; sorted by (space, surface)
    std::vector<std::string> dangling_aliases;      // alias keys nothing claims; sorted
    std::vector<std::string> slot_aliases;          // alias keys only a service/hook point claims
    std::vector<std::string> ambiguous_identities;  // fatal; sorted

    // surface_name -> permission_key for every resolved TOOL. The permission key
    // is the non-alias: the bare original name when no other registration
    // contests it, the stable identity ("plugin.name") otherwise -- so
    // per_tool_overrides / allowed_tools never reference what the model sees.
    std::unordered_map<std::string, std::string> permission_keys;
};

// One entry per logical registration, always under its ORIGINAL name: core
// built-ins under an empty plugin name, plugin registrations under their
// plugin's name, aliased ones still under the name they registered with.
//
// `core_tool_names` reserves names the harness itself registers LATER, at serve
// stage, after this check has run -- `spawn_subagent` is the one -- so a plugin
// claiming one is a diagnosed collision rather than a silent shadow.
//
// Each alias applies to every name namespace claiming its key (a plugin
// shipping a tool and a command under one name aliases both, matching what
// ToolRegistry::ApplyAliases and CommandRegistry each do).
RegistrationNames ResolveRegistrationNames(const tools::ToolRegistry& registry,
                                           const std::map<std::string, std::string>& aliases,
                                           const commands::CommandRegistry* commands = nullptr,
                                           const std::vector<std::string>& core_tool_names = {},
                                           const ServiceRegistry* services = nullptr,
                                           const hooks::HookDispatcher* hook_points = nullptr);

}  // namespace minimal::plugins
