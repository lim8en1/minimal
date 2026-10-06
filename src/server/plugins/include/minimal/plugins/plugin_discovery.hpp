#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/plugin_loader.hpp"

namespace minimal::plugins {

// Parsed plugins.json.
//
// Note this is an OBJECT, not the bare array an earlier draft of the design
// described: that draft also placed the aliases at the top level, and both
// cannot be true at once.
//
//   {
//     "plugins": [ {"file": "tools-fs.so", "provenance": "bundled", "enabled": true} ],
//     "aliases": { "bash_over_ssh.bash": "ssh_bash" }
//   }
//
// A plugin's own settings are NOT here: they live in plugins.d/<stem>.json,
// forwarded to it at registration.
// The plugins.json document: entries keyed by (provenance, file), aliases, and
// every other top-level key carried through untouched. The format and its
// parse/serialize/discover logic are shared with the client host
// (minimal/common/plugin_config.hpp); this file only supplies what is
// server-specific (the file label and which bundled plugins ship disabled).
using PluginConfig = common::PluginConfigFile;

PluginConfig ParsePluginConfig(const std::string& json_text);
std::string SerializePluginConfig(const PluginConfig& config);

// Directory holding the bundled plugins, resolved at runtime relative to the
// running executable rather than from the compile-time install prefix -- that
// prefix describes where `make install` puts things, not where this binary
// actually is, and an uninstalled build tree must work too.
std::filesystem::path BundledPluginDir();

// Scans both directories, merges against an existing plugins.json, and applies
// the provenance rule to anything newly discovered: bundled defaults to ENABLED,
// user defaults to DISABLED. Returns candidates in a stable order (bundled
// first, then user, each alphabetical) so load order is reproducible.
//
// `config` is updated in place with newly discovered entries so the caller can
// write it back. Each enabled candidate carries `plugin_config_dir`/<stem>.json
// (the plugins.d dir; empty = no per-plugin files, every config {}).
std::vector<PluginCandidate> DiscoverPlugins(const std::filesystem::path& bundled_dir,
                                              const std::filesystem::path& user_dir,
                                              PluginConfig& config,
                                              const std::filesystem::path& plugin_config_dir = {});

}  // namespace minimal::plugins
