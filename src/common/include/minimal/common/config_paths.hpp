#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::common {

// getenv, but treats an empty value as unset -- an exported-but-empty env var
// (XDG_CONFIG_HOME, an MCP secret referenced via ${VAR}, providers.json's
// api_key_env) is common and must not be treated as a resolved value. The one
// shared "is this env var actually set" primitive; every env-var lookup in
// the tree should go through this rather than calling getenv directly.
std::optional<std::string> LookupEnv(std::string_view name);

// Resolution order:
//   1. $MINIMAL_CONFIG_DIR   -- explicit escape hatch
//   2. $XDG_CONFIG_HOME/minimal
//   3. $HOME/.config/minimal -- terminal-tool convention (git/gh/rg), deliberately
//      not macOS's GUI ~/Library/Application Support
//
// A relative value is rejected at EVERY step, the escape hatch included; it
// used to resolve against the cwd -- which for a spawned server (chdir'd to /)
// meant a different directory than the client's. Each step is independent: an
// absolute $XDG_CONFIG_HOME answers even when $HOME is unset, and only step 3
// needs $HOME (it throws when there is none). Shared by server and client: the
// client reads themes/ and keybindings.json from the same directory the
// server resolves.
std::filesystem::path ConfigDir();

// Mutable runtime state (SQLite session store, logs) lives here, NOT under the
// config dir: $MINIMAL_DATA_DIR (the same escape hatch MINIMAL_CONFIG_DIR is),
// else $XDG_DATA_HOME/minimal, else $HOME/.local/share/minimal. Relative values
// rejected and $HOME needed only for the last step, as above.
std::filesystem::path DataDir();

}  // namespace minimal::common
