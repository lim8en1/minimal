#pragma once

// `minimal-server plugin ...` -- reading and editing plugins.d/<name>.json
// from a shell, against each plugin's own schema (minimal_plugin_config_schema)
// and validator. `minimal plugin ...` runs this (see the client's
// plugin_command.cpp): only the server side can open a server plugin.
//
//   plugin                         list: name, enabled, how many keys are set
//   plugin <name>                  every field: value (default or set), apply, doc
//   plugin get <name> <key>        one value
//   plugin set <name> <key> <value>
//   plugin unset <name> <key>
//   --json                         machine-readable output (list, show, get)
//
// Opens plugins for their config symbols only (InspectPluginConfig): no
// registration, no storage, never writes plugins.json -- safe beside a
// running server.

#include <filesystem>
#include <string>
#include <vector>

namespace minimal::server {

// `args`: everything after "plugin". Returns the process exit code: 0, 1 for
// a refused operation (unknown plugin or key, a value the plugin rejects),
// 2 for a usage error.
int RunPluginCli(const std::vector<std::string>& args, const std::filesystem::path& config_dir);

}  // namespace minimal::server
