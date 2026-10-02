#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/client_plugin_loader.hpp"

namespace minimal::client::plugins {

// Parsed client-plugins.json -- the CLIENT twin of the server's plugins.json,
// identical in shape (one file format, two owners: the server owns plugins.json,
// the client owns client-plugins.json, so neither writer can stomp the other's
// entries):
//
//   {
//     "plugins": [ {"file": "todo.so", "provenance": "bundled", "enabled": true,
//                   "config": {...}} ],
//     "aliases": { "myplugin.widget": "widget" }
//   }
//
// Everything unmodelled round-trips through `preserved`, because this file is
// rewritten on every start -- the same rule the server file follows.
// client-plugins.json: the same document shape as the server's plugins.json, and
// the same implementation (minimal/common/plugin_config.hpp).
using ClientPluginConfig = common::PluginConfigFile;

ClientPluginConfig ParseClientPluginConfig(const std::string& json_text);
std::string SerializeClientPluginConfig(const ClientPluginConfig& config);

// Scans the bundled client dir (recursive: build tree stages one subdir per
// plugin) and the user dir (flat), merges against an existing
// client-plugins.json (updated in place so the caller can write it back),
// applies the provenance rule to new entries (bundled ENABLED, user DISABLED)
// and returns candidates in stable load order (bundled first, then user, each
// alphabetical).
std::vector<PluginCandidate> DiscoverClientPlugins(
    const std::filesystem::path& bundled_dir, const std::filesystem::path& user_dir,
    ClientPluginConfig& config);

}  // namespace minimal::client::plugins
