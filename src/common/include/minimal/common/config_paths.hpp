#pragma once

#include <filesystem>
#include <string_view>

namespace minimal::common {

// Resolution order:
//   1. $MINIMAL_CONFIG_DIR   -- explicit escape hatch
//   2. $XDG_CONFIG_HOME/minimal
//   3. $HOME/.config/minimal -- terminal-tool convention (git/gh/rg), deliberately
//      not macOS's GUI ~/Library/Application Support
//
// A relative value is rejected at EVERY step, the escape hatch included: a
// spawned server (chdir'd to /) and the client would resolve it against
// different directories. Each step is independent: an
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

// gRPC's scheme for a unix-domain socket target ("unix://<path>").
inline constexpr std::string_view kUnixSocketScheme = "unix://";

// The server's default socket, under DataDir(): what the server listens on
// and the client dials when neither is told otherwise.
inline std::filesystem::path DefaultSocketPath() { return DataDir() / "server.sock"; }

}  // namespace minimal::common
