// tools-bash -- the bash tool, plus the /restart-shell command, plus a
// SessionObserver.
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

#include "minimal/common/abi_tag.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-bash"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

// TODO: minimal_plugin_register_tools           -- bash (allowlist, no sh -c,
//         metacharacters rejected not sanitized, posix_spawn/argv, setrlimit +
//         killpg, stdout streamed via ToolContext::emit_output)
// TODO: minimal_plugin_register_commands        -- /restart-shell
// TODO: minimal_plugin_register_session_observer -- OnSessionEnded frees cwd/env

}  // extern "C"
