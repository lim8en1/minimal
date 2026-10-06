#pragma once

// `minimal plugin ...`: plugin configuration from a shell. With a server
// running, set/unset go through its `plugins` endpoint: validated in the
// server's own process and environment (an "env:" reference resolves against
// its variables, not this shell's), written, and applied. Everything else --
// and every edit when no server runs -- happens in `minimal-server plugin ...`
// (src/server/plugin_cli.hpp; only the server side can open a server plugin
// for its schema), run as a child with the terminal passed through.

#include <string>
#include <vector>

namespace minimal::client {

// `args`: everything after "plugin". Returns the process exit code.
int RunPluginCommand(const std::vector<std::string>& args);

}  // namespace minimal::client
