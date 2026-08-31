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
// Each entry may also carry a "config" object -- the plugin's own settings,
// forwarded to it at registration.
struct PluginConfig {
    struct Entry {
        std::string file;  // basename, resolved against its provenance directory
        Provenance provenance = Provenance::User;
        bool enabled = false;
        // This plugin's own configuration, verbatim. Source: the optional
        // "config" object on the entry:
        //   {"plugins": [{"file": "tools-bash.so", "enabled": true,
        //                 "config": {"timeout_ms": 30000}}]}
        // Core imposes no schema -- each plugin owns the shape of its own
        // slice, the same rule as tools_settings. A non-object value degrades
        // to an empty object with a startup warning.
        nlohmann::json config = nlohmann::json::object();
    };
    std::vector<Entry> plugins;

    // Fully-qualified "plugin.original_name" -> surface name. Applied at
    // registration, before collision checking, so aliasing is itself a valid way
    // to resolve a collision. Key is `aliases` in plugins.json -- the old
    // `command_aliases` name was a lie, since the one namespace aliases TOOLS
    // as well as commands (one flat plugin.name-keyed space; a plugin shipping
    // both a tool and a command under one original name aliases them together
    // or not at all).
    std::map<std::string, std::string> aliases;

    // EVERY OTHER TOP-LEVEL KEY, carried through untouched. This file is
    // rewritten on every start (discovery persists what it found), and the
    // serializer reconstructs the document from these fields -- so anything it
    // did not know about was silently deleted on the next boot, in a file the
    // user is explicitly invited to edit. Round-tripping is cheaper than a
    // migration story for keys we have not invented yet.
    nlohmann::json preserved = nlohmann::json::object();
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
