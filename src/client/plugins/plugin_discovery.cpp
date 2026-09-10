#include "minimal/client/plugins/plugin_discovery.hpp"

#include <algorithm>
#include <set>

#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {
namespace {

constexpr const char* kBundled = "bundled";
constexpr const char* kUser = "user";

bool IsPluginFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

}  // namespace

// Parse/serialize/discover are structural twins of the server's
// plugin_discovery.cpp rather than shared code: the server pieces live in
// server core (which the client cannot link), and the FILE FORMAT is the
// contract both sides implement. When the shapes drift, the drift shows up
// here as a diff against that file.

ClientPluginConfig ParseClientPluginConfig(const std::string& json_text) {
    ClientPluginConfig config;
    if (json_text.empty()) return config;

    const auto parsed = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        config.malformed = true;   // the host warns and skips the rewrite
        return config;
    }

    if (auto it = parsed.find("plugins"); it != parsed.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            ClientPluginConfig::Entry entry;
            // Type-checked reads, not item.value(key, default): that overload
            // only substitutes the default when the key is ABSENT -- a key
            // present with the wrong JSON type (a user hand-editing this file
            // can write "enabled": "false" or "enabled": 1) makes it throw
            // type_error.302 uncaught, aborting the TUI at startup.
            // Degrade-don't-throw, like the rest of this file (see 'config'
            // below).
            if (auto file_it = item.find("file"); file_it != item.end()) {
                if (file_it->is_string()) {
                    entry.file = file_it->get<std::string>();
                } else {
                    common::DefaultLogger().warn(
                        "client-plugins.json: a plugin entry has a non-string 'file' ({}); "
                        "skipping it",
                        file_it->type_name());
                }
            }
            if (entry.file.empty()) continue;
            entry.provenance = Provenance::User;
            if (auto prov_it = item.find("provenance");
                prov_it != item.end() && prov_it->is_string()) {
                entry.provenance =
                    prov_it->get<std::string>() == kBundled ? Provenance::Bundled : Provenance::User;
            }
            entry.enabled = false;
            if (auto enabled_it = item.find("enabled"); enabled_it != item.end()) {
                if (enabled_it->is_boolean()) {
                    entry.enabled = enabled_it->get<bool>();
                } else {
                    common::DefaultLogger().warn(
                        "client-plugins.json: entry '{}' has a non-boolean 'enabled' ({}); "
                        "defaulting to disabled",
                        entry.file, enabled_it->type_name());
                }
            }
            if (auto config_it = item.find("config"); config_it != item.end()) {
                if (config_it->is_object()) {
                    entry.config = *config_it;
                } else {
                    // Degrade-don't-throw, like the rest of this file.
                    common::DefaultLogger().warn(
                        "client-plugins.json: entry '{}' has a non-object 'config' ({}); "
                        "ignoring it -- the plugin will receive an empty config",
                        entry.file, config_it->type_name());
                }
            }
            config.plugins.push_back(std::move(entry));
        }
    }

    if (auto it = parsed.find("aliases"); it != parsed.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) config.aliases.emplace(key, value.get<std::string>());
        }
    }

    for (const auto& [key, value] : parsed.items()) {
        if (key == "plugins" || key == "aliases") continue;
        config.preserved[key] = value;
    }
    return config;
}

std::string SerializeClientPluginConfig(const ClientPluginConfig& config) {
    // Starts from what was read, so unmodelled keys survive the round trip.
    nlohmann::json out = config.preserved;
    out["plugins"] = nlohmann::json::array();
    for (const auto& entry : config.plugins) {
        nlohmann::json item = {
            {"file", entry.file},
            {"provenance", entry.provenance == Provenance::Bundled ? kBundled : kUser},
            {"enabled", entry.enabled},
        };
        item["config"] = entry.config;
        out["plugins"].push_back(std::move(item));
    }
    if (!config.aliases.empty()) out["aliases"] = nlohmann::json(config.aliases);
    return out.dump(2) + "\n";
}

std::vector<PluginCandidate> DiscoverClientPlugins(
    const std::filesystem::path& bundled_dir, const std::filesystem::path& user_dir,
    ClientPluginConfig& config) {
    struct Found {
        std::filesystem::path path;
        std::string file;
        Provenance provenance;
    };
    std::vector<Found> found;

    auto scan = [&](const std::filesystem::path& dir, Provenance provenance, bool recurse) {
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
        if (recurse) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec) && IsPluginFile(entry.path())) {
                    found.push_back({entry.path(), entry.path().filename().string(), provenance});
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec) && IsPluginFile(entry.path())) {
                    found.push_back({entry.path(), entry.path().filename().string(), provenance});
                }
            }
        }
    };

    scan(bundled_dir, Provenance::Bundled, /*recurse=*/true);
    scan(user_dir, Provenance::User, /*recurse=*/false);

    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.provenance != b.provenance) return a.provenance == Provenance::Bundled;
        return a.file < b.file;
    });

    std::vector<PluginCandidate> candidates;
    candidates.reserve(found.size());

    for (const auto& item : found) {
        // Keyed by (provenance, file), NOT file alone -- ported from the
        // server's DiscoverPlugins: a user-dir plugin that happens to share a
        // basename with a bundled one (a dropped-in logo.so) used to match
        // the bundled entry and inherit its enabled:true, dlopening and
        // running a plugin nobody opted into. Two independent entries let
        // the user toggle each side separately, which is also what the
        // collision warning below tells them to do.
        auto existing = std::find_if(config.plugins.begin(), config.plugins.end(),
                                     [&](const ClientPluginConfig::Entry& e) {
                                         return e.file == item.file &&
                                                e.provenance == item.provenance;
                                     });

        if (existing == config.plugins.end()) {
            // Provenance decides the default, mechanically: shipped-with-this-
            // build is trusted, dropped-in-later is opt-in.
            ClientPluginConfig::Entry entry;
            entry.file = item.file;
            entry.provenance = item.provenance;
            entry.enabled = (item.provenance == Provenance::Bundled);
            config.plugins.push_back(entry);
            existing = std::prev(config.plugins.end());
        }

        // The same basename discovered from BOTH directories: both files
        // still load (each under its own now-independent entry), which is a
        // guaranteed name collision the user cannot see coming from
        // client-plugins.json alone. Warned at discovery, like the server's.
        const bool also_from_other_provenance = std::any_of(
            config.plugins.begin(), config.plugins.end(),
            [&](const ClientPluginConfig::Entry& e) {
                return e.file == item.file && e.provenance != item.provenance;
            });
        if (also_from_other_provenance) {
            common::DefaultLogger().warn(
                "client plugin '{}' exists both bundled and in the user plugin dir; both will "
                "load and the names will collide -- disable one in client-plugins.json",
                item.file);
        }

        candidates.push_back(
            PluginCandidate{item.path, item.provenance, existing->enabled, existing->config});
    }

    return candidates;
}

}  // namespace minimal::client::plugins
