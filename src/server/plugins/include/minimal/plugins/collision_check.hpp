#pragma once

// Runs NameRegistry over what actually got registered.
//
// NameRegistry has existed and been unit-tested since the plugin work landed,
// but nothing ever CONSTRUCTED one outside its own tests -- so the rule
// ARCHITECTURE.md calls fatal ("two plugins registering the same tool name,
// command name, ViewSpec::id or ToolResultRenderer::tool_name halt startup")
// was never enforced, and ToolRegistry::Register quietly shadowed duplicates
// via insert_or_assign. `command_aliases` was likewise parsed from plugins.json
// and re-serialized without ever being applied to anything.
//
// The command system is the first track with a SECOND registry, which is what
// makes the abstraction worth connecting rather than reimplementing per registry.

#include <map>
#include <string>
#include <vector>

#include "minimal/commands/command_registry.hpp"
#include "minimal/plugins/name_registry.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

// One duplicated surface name, plus WHO produced it. NameRegistry reports the
// name alone; ARCHITECTURE.md requires "a diagnostic naming both", because the
// name by itself does not tell you which plugin to disable or alias -- and the
// whole point of making this fatal is that the user has to choose deliberately.
struct Collision {
    std::string kind;         // "tool" or "command"
    std::string surface_name;
    std::vector<std::string> claimed_by;  // stable identities, "plugin.original"
};

struct CollisionReport {
    std::vector<Collision> fatal;
    std::vector<std::string> dangling_aliases;   // an alias key naming nothing

    bool ok() const { return fatal.empty() && dangling_aliases.empty(); }
};

CollisionReport CheckRegistrationNames(const tools::ToolRegistry& tools,
                                       const commands::CommandRegistry& commands,
                                       const std::map<std::string, std::string>& command_aliases);

// Multi-line, one finding per line, already indented for a startup diagnostic.
std::string DescribeCollisions(const CollisionReport& report);

}  // namespace minimal::plugins
