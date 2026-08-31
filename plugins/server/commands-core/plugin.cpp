// commands-core -- the built-in /-commands, as one plugin rather than nine.
//
// Deliberately does NOT include /restart-shell: that touches tools-bash's
// per-session state and therefore belongs to tools-bash. A plugin owns the
// commands that touch its own state.

#include "core_commands.hpp"

#include "minimal/commands/command_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/plugins/plugin_abi.h"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "commands-core"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    for (auto& command : minimal::commands_core::MakeCoreCommands()) {
        registry.Register(std::move(command));
    }
    // Last, and holding a reference to the registry: /help lists what is
    // registered when it RUNS, so a plugin loading after this one still appears.
    registry.Register(minimal::commands_core::MakeHelpCommand(registry));
}

}  // extern "C"
