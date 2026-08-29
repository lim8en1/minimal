#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "minimal/plugins/plugin_loader.hpp"

namespace minimal::plugins {

// Parsed plugins.json.
//
// Note this is an OBJECT, not the bare array an earlier draft of the design
// described: that draft also placed `command_aliases` at the top level, and both
// cannot be true at once.
//
//   {
//     "plugins": [ {"file": "tools-fs.so", "provenance": "bundled", "enabled": true} ],
//     "command_aliases": { "bash_over_ssh.bash": "ssh_bash" }
//   }
struct PluginConfig {
    struct Entry {
        std::string file;  // basename, resolved against its provenance directory
        Provenance provenance = Provenance::User;
        bool enabled = false;
    };
    std::vector<Entry> plugins;

    // Fully-qualified "plugin.original_name" -> surface name. Applied at
    // registration, before collision checking, so aliasing is itself a valid way
    // to resolve a collision.
    std::map<std::string, std::string> command_aliases;
};

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
// write it back.
std::vector<PluginCandidate> DiscoverPlugins(const std::filesystem::path& bundled_dir,
                                              const std::filesystem::path& user_dir,
                                              PluginConfig& config);

}  // namespace minimal::plugins
