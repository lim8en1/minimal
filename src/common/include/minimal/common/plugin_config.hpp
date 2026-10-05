#pragma once

// The plugins.json / client-plugins.json file format and plugin-file discovery,
// shared by the server and client plugin hosts. The two files are the same
// document shape (an `enabled`-toggled list keyed by (provenance, file), aliases,
// and any other top-level key carried through untouched); they used to be
// structural twins that drifted -- a fix on one side (non-throwing directory
// iteration, type-checked reads, warnings for a wrongly-typed `plugins` /
// `aliases`) would land on the other only by hand. This is the one
// implementation; each host passes a PluginConfigFormat for what genuinely
// differs (the file's name for messages, which bundled plugins ship disabled,
// extra "this key was renamed" checks).
//
// Lives in common because the client cannot link server core.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::common {

enum class PluginProvenance { Bundled, User };

// What plugin discovery hands the loader for one plugin file.
struct PluginFileCandidate {
    std::filesystem::path file;
    PluginProvenance provenance = PluginProvenance::User;
    bool enabled = false;
    // The plugin's own configuration, verbatim from its entry's "config" object.
    // Core imposes no schema -- each plugin owns the shape of its own slice.
    nlohmann::json config = nlohmann::json::object();
};

struct PluginConfigFile {
    struct Entry {
        std::string file;  // basename, resolved against its provenance directory
        PluginProvenance provenance = PluginProvenance::User;
        bool enabled = false;
        nlohmann::json config = nlohmann::json::object();
    };
    std::vector<Entry> plugins;

    // Fully-qualified "plugin.original_name" -> surface name. Applied at
    // registration, before collision checking, so aliasing is itself a valid way
    // to resolve a collision.
    std::map<std::string, std::string> aliases;

    // EVERY OTHER TOP-LEVEL KEY, carried through untouched. The file is rewritten
    // on every start (discovery persists what it found), and the serializer
    // rebuilds the document from these fields -- anything it did not know about
    // would be silently deleted on the next boot, in a file the user is invited
    // to edit.
    nlohmann::json preserved = nlohmann::json::object();

    // Set when the text could not be parsed at all (invalid JSON or a non-object
    // top level). The caller must warn and skip the rewrite: persisting the
    // degraded-to-empty default over the user's actual bytes would destroy every
    // enablement decision and per-plugin config in a hand-edited file.
    bool malformed = false;
};

// What differs between hosts.
struct PluginConfigFormat {
    // The file's name, for warnings ("plugins.json", "client-plugins.json").
    std::string file_label = "plugins.json";
    // "plugin" / "client plugin", for the discovery warnings.
    std::string noun = "plugin";
};

// A bundled plugin that ships present but DISABLED (it spends the user's
// tokens or data on its own initiative) says so itself: its build drops this
// marker beside the module (minimal_add_plugin(... DISABLED_BY_DEFAULT)), so
// no host keeps a list of plugin names. Consulted only when a file has no
// plugins.json entry yet.
inline constexpr const char* kDisabledByDefaultSuffix = ".disabled-by-default";

PluginConfigFile ParsePluginConfigFile(const std::string& json_text,
                                       const PluginConfigFormat& format);
std::string SerializePluginConfigFile(const PluginConfigFile& config);

// Finds the plugin files under `bundled_dir` (recursively, so the build tree's
// one-subdirectory-per-plugin layout works like the flat installed one) and
// `user_dir` (flat), adds an entry to `config` for each one it has not seen
// (bundled: enabled unless the format says otherwise; user: disabled), and
// returns one candidate per file, bundled first. Never throws on a filesystem
// error: a mid-scan failure just ends that scan early.
std::vector<PluginFileCandidate> DiscoverPluginFiles(const std::filesystem::path& bundled_dir,
                                                     const std::filesystem::path& user_dir,
                                                     PluginConfigFile& config,
                                                     const PluginConfigFormat& format);

}  // namespace minimal::common
