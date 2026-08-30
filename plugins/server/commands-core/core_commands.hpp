#pragma once

// The built-in /-commands, as one plugin rather than nine.
//
// Deliberately does NOT include /restart-shell: that touches tools-bash's
// per-session cwd/env, and one .so cannot reach into another's state. A plugin
// owns the commands that touch its own state -- which is why
// minimal_plugin_register_tools and ..._register_commands are independent
// optional symbols rather than one "kind" of plugin.
//
// /new, /attach and /sessions are absent too, for a different reason: they drive
// a tab model that arrives with the TUI (track E), and a command that creates a
// session no client can attach to would be surface built ahead of its consumer.

#include <memory>
#include <vector>

#include "minimal/commands/command.hpp"

namespace minimal::commands {
class CommandRegistry;
}  // namespace minimal::commands

namespace minimal::commands_core {

// Everything this plugin registers. A free function rather than a registration
// call so the same list can be built in a test without a CommandRegistry.
std::vector<std::unique_ptr<minimal::commands::Command>> MakeCoreCommands();

// Separate because /help lists the registry it lives in, so it needs a reference
// to the thing it is about to be inserted into.
std::unique_ptr<minimal::commands::Command> MakeHelpCommand(
    const minimal::commands::CommandRegistry& registry);

}  // namespace minimal::commands_core
