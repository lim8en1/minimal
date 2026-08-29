// commands-core -- the built-in /-commands, as one plugin rather than nine.
//
// Deliberately does NOT include /restart-shell: that touches tools-bash's
// per-session state and therefore belongs to tools-bash. A plugin owns the
// commands that touch its own state.

#include "minimal/common/abi_tag.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "commands-core"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

// TODO: minimal_plugin_register_commands --
//   /new /attach /sessions /agent /model /permission-mode /compact /clear
//   /providers /help
// /compact is an LLMCommand (issues a real provider request); the rest are
// HarnessCommands, and the read-only ones may run concurrently with a live turn.

}  // extern "C"
