#pragma once

#include <map>
#include <string>
#include <unordered_map>

#include "minimal/plugins/name_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

// The three-naming-levels rule (ARCHITECTURE.md, "Name collisions and aliases")
// applied to everything registered so far: one feed of NameRegistry from the
// tool AND command attributions plus the plugins.json aliases, resolved.
//
// One function over both kinds rather than one per registry, because the rule is
// one rule -- and because a second copy is exactly the drift the fatal-collision
// rule exists to prevent. NameRegistry keeps the two namespaces separate
// internally, so a tool and a command may share a surface name without colliding.
struct RegistrationNames {
    NameRegistry::Result result;  // entries, fatal_collisions, dangling_aliases

    // surface_name -> permission_key for every resolved TOOL -- commands have no
    // permission keys, so they are excluded even though they share the pass. The
    // executor and
    // the allowed-tools filter use this so per_tool_overrides / allowed_tools
    // reference the non-alias key (bare when unambiguous, qualified when two
    // plugins provide the original name) rather than what the model sees.
    std::unordered_map<std::string, std::string> permission_keys;
};

// Feeds NameRegistry from `registry.attributions()` and, when given, the command
// registry's -- one entry per logical registration, always carrying the ORIGINAL
// name: core built-ins under an empty plugin name, plugin registrations under
// their plugin's name, aliased ones still under the name they registered with.
//
// Each key in `aliases` is routed to the kind that actually claims it, so a
// command alias is not reported dangling merely because no tool answers to it.
// A key no registration of either kind claims lands in dangling_aliases once.
//
// Collision and dangling lists are sorted: NameRegistry iterates unordered
// maps, and startup diagnostics must be stable run to run.
RegistrationNames ResolveRegistrationNames(const tools::ToolRegistry& registry,
                                           const std::map<std::string, std::string>& aliases,
                                           const commands::CommandRegistry* commands = nullptr);

}  // namespace minimal::plugins
