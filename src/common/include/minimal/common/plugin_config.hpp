#pragma once

// The plugins.json / client-plugins.json file format, the per-plugin config
// files beside them (plugins.d/, client-plugins.d/), and plugin-file discovery,
// shared by the server and client plugin hosts. The two files are the same
// document shape (an `enabled`-toggled list keyed by (provenance, file), aliases,
// and any other top-level key carried through untouched), so there is one
// implementation and fixes cannot drift between hosts; each host passes a PluginConfigFormat for what genuinely
// differs (the file's name for messages, which bundled plugins ship disabled,
// extra "this key was renamed" checks).
//
// Lives in common because the client cannot link server core.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/plugin_config_label.hpp"

namespace minimal::common {

enum class PluginProvenance { Bundled, User };

// What plugin discovery hands the loader for one plugin file.
struct PluginFileCandidate {
    std::filesystem::path file;
    PluginProvenance provenance = PluginProvenance::User;
    bool enabled = false;
    // The plugin's own configuration, verbatim from its plugins.d file ({}
    // when the file does not exist). Core imposes no schema -- each plugin
    // owns the shape of its own file.
    nlohmann::json config = nlohmann::json::object();
    // Set when the plugins.d file exists but could not be read (invalid JSON,
    // not an object, unreadable), fully worded. The loader fails the plugin
    // with it -- the same outcome as the plugin's own parser refusing a value.
    std::optional<std::string> config_error;
};

struct PluginConfigFile {
    struct Entry {
        std::string file;  // basename, resolved against its provenance directory
        // Absent in a hand-written entry: discovery resolves it by file name
        // (an error when both directories ship that name), then persists it.
        std::optional<PluginProvenance> provenance;
        // Absent: the discovery default (bundled on unless marked
        // disabled-by-default, user off), persisted once resolved.
        std::optional<bool> enabled;
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
    // enablement decision in a hand-edited file.
    bool malformed = false;
    // Per-entry problems (a wrong type, an unknown field, an ambiguous
    // provenance), fully worded. Never silently defaulted: a plugin the user
    // disabled must not come back because its entry had a typo. The server
    // treats any as fatal; the client warns.
    std::vector<std::string> problems;
    // The entries that had a problem but still named their file: basename ->
    // their "enabled" when that much read as a boolean. Discovery gives such
    // a file this value -- or DISABLED -- rather than the default: a plugin
    // the user turned off must not come back because its entry has a typo.
    std::map<std::string, std::optional<bool>> broken;
};

// What differs between hosts.
struct PluginConfigFormat {
    // The file's name, for warnings ("plugins.json", "client-plugins.json").
    std::string file_label = "plugins.json";
    // "plugin" / "client plugin", for the discovery warnings.
    std::string noun = "plugin";
    // The per-plugin config directory's name (kPluginConfigDirName /
    // kClientPluginConfigDirName), for messages, and where it is. An empty
    // path reads no per-plugin files: every candidate gets {}.
    std::string config_dir_label = kPluginConfigDirName;
    std::filesystem::path config_dir;
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
// returns one candidate per file, bundled first, each carrying its
// format.config_dir/<stem>.json. Never throws on a filesystem error: a
// mid-scan failure just ends that scan early.
std::vector<PluginFileCandidate> DiscoverPluginFiles(const std::filesystem::path& bundled_dir,
                                                     const std::filesystem::path& user_dir,
                                                     PluginConfigFile& config,
                                                     const PluginConfigFormat& format);

}  // namespace minimal::common
