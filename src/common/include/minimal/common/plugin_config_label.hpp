#pragma once

// Where a plugin's own configuration lives, and the label its messages carry.
// An SDK header: plugins label their config::Reader with PluginConfigLabel.
//
// Per-plugin configuration is one file per plugin, beside the host's list
// file: <config dir>/plugins.d/<stem>.json (server) and
// <config dir>/client-plugins.d/<stem>.json (client), where <stem> is the
// module's file name without its extension -- the plugin's name for every
// bundled plugin. The list file (plugins.json) says only WHICH plugins load.

#include <string>
#include <string_view>

namespace minimal::common {

inline constexpr const char* kPluginConfigDirName = "plugins.d";
inline constexpr const char* kClientPluginConfigDirName = "client-plugins.d";

// "plugins.d/tools-web.json" -- the label a plugin's config messages start
// with, the same relative spelling as "plugins.json".
inline std::string PluginConfigLabel(std::string_view dir_name, std::string_view stem) {
    return std::string(dir_name) + "/" + std::string(stem) + ".json";
}

}  // namespace minimal::common
