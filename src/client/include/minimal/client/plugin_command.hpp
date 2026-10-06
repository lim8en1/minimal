#pragma once

// `minimal plugin ...`: plugin configuration from a shell. The work happens in
// `minimal-server plugin ...` (src/server/plugin_cli.hpp) -- only the server
// side can open a server plugin for its schema -- which this runs as a child,
// passing the terminal through. After a successful set/unset it tells a
// running server (if there is one) to apply the change.

#include <string>
#include <vector>

namespace minimal::client {

// `args`: everything after "plugin". Returns the process exit code.
int RunPluginCommand(const std::vector<std::string>& args);

}  // namespace minimal::client
