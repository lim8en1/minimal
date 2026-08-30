#pragma once

#include <map>
#include <string>
#include <unordered_map>

#include "minimal/plugins/name_registry.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

// The three-naming-levels rule (ARCHITECTURE.md, "Name collisions and aliases")
// applied to everything registered so far: one feed of NameRegistry from the
// tool attributions plus the plugins.json aliases, resolved.
struct ToolNameResolution {
    NameRegistry::Result result;  // entries, fatal_collisions, dangling_aliases

    // surface_name -> permission_key for every resolved tool. The executor and
    // the allowed-tools filter use this so per_tool_overrides / allowed_tools
    // reference the non-alias key (bare when unambiguous, qualified when two
    // plugins provide the original name) rather than what the model sees.
    std::unordered_map<std::string, std::string> permission_keys;
};

// Feeds NameRegistry from `registry.attributions()` -- one entry per logical
// tool, always carrying the ORIGINAL name: core built-ins under an empty plugin
// name, plugin tools under their plugin's name, aliased tools still under the
// name they registered with (ToolRegistry::Rename keeps the attribution intact).
//
// Every key in `aliases` is applied as a Tool-kind alias; a key that matches no
// registered identity comes back in result.dangling_aliases. Commands do not
// exist yet, so tool is the only kind to try -- when the command system lands,
// extend this to fall through to NameKind::Command for identities tools do not
// claim.
//
// Collision and dangling lists are sorted: NameRegistry iterates unordered
// maps, and startup diagnostics must be stable run to run.
ToolNameResolution ResolveToolNames(const tools::ToolRegistry& registry,
                                    const std::map<std::string, std::string>& aliases);

}  // namespace minimal::plugins
