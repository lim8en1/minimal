#pragma once

// The built-in /-commands, as one plugin rather than nine.
//
// Deliberately does NOT include /restart-shell: it resets tools-bash's
// per-session shell state, and a command belongs to the plugin that owns the
// state it touches. Other plugins can now ASK for that state through the
// BashSessionStateService tools-bash provides (docs/EXTENSIBILITY.md §2.2);
// that makes the state reachable, not the command relocatable.
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
