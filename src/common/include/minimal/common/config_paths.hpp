#pragma once

#include <filesystem>

namespace minimal::common {

// Resolution order:
//   1. $MINIMAL_CONFIG_DIR   -- explicit escape hatch
//   2. $XDG_CONFIG_HOME/minimal
//   3. $HOME/.config/minimal -- terminal-tool convention (git/gh/rg), deliberately
//      not macOS's GUI ~/Library/Application Support
//
// Shared by server and client: the client reads themes/ and keybindings.json from
// the same directory the server resolves.
std::filesystem::path ConfigDir();

// Mutable runtime state (SQLite session store, logs) lives here, NOT under the
// config dir: $XDG_DATA_HOME/minimal, else $HOME/.local/share/minimal.
std::filesystem::path DataDir();

}  // namespace minimal::common
