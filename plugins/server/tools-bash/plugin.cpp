// tools-bash -- the bash tool, plus (when their core subsystems land) the
// /restart-shell command and a SessionObserver.
//
// All three live together deliberately. The per-session cwd/env map lives INSIDE
// this plugin (core holds no bash state), so:
//   - /restart-shell must be registered here, not in commands-core: one .so cannot
//     reach into another's state.
//   - OnSessionEnded must be implemented here, or that map grows for the lifetime
//     of the process.
// This is the canonical example of plugin-owned per-session state needing a
// lifecycle signal.
//
// Split from tools-fs because it has a distinct security profile and is the one
// tool with a plausible alternate implementation (container / remote executor)
// someone would actually swap. Replacing it needs no new mechanism: disable this
// plugin and ship one registering `bash`.
//
// SECURITY MODEL: commands run through /bin/sh -c, and the permission gate is the
// boundary -- bash is category Execute (Ask by default) and the operator approves
// the command. There is no allowlist and no attempt at one: keyed on name it dies
// to `cp /bin/bash /tmp/git`, keyed on path or hash it dies to the interpreters
// you must allow anyway (git aliases, make, xargs, env). The threat model is model
// ERROR, not model malice; against an adversarial model holding a shell only a
// container helps, which is what the swap-out path above is for.
//
// Consent scoping (see ARCHITECTURE.md) makes Ask livable: an invocation discloses
// the binaries it names, by resolved path, and the operator may grant them for the
// session. That reduces prompts; it does not constrain what a command can do.

#include "minimal/plugins/plugin_abi.h"

#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"

namespace {

// One store for the whole plugin: the tool, the future /restart-shell command,
// and the future SessionObserver all operate on this same map. Process-lifetime
// (plugins are never unloaded), matching the registry objects it feeds.
minimal::tools_bash::BashSessionStateStore& BashStore() {
    static minimal::tools_bash::BashSessionStateStore store;
    return store;
}

}  // namespace

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-bash"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::ToolRegistrationContext& context) {
    // Core hands over the whole `tools` object without imposing a schema; this
    // plugin owns the shape of its own slice.
    const auto settings = minimal::tools_bash::ParseBashSettings(context.tools_settings);
    registry.Register(minimal::tools_bash::MakeBashTool(settings, BashStore()));
}

// TODO: minimal_plugin_register_commands        -- /restart-shell (resets the
//         same map via BashStore()). Lands with the core commands subsystem.
// TODO: minimal_plugin_register_session_observer -- OnSessionEnded calls
//         BashStore().Erase(session_id). Lands with the observer registry;
//         until then no session lifecycle exists to leak from.

}  // extern "C"
