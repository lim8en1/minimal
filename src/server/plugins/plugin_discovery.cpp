#include "minimal/plugins/plugin_discovery.hpp"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::plugins {
namespace {

constexpr const char* kBundled = "bundled";
constexpr const char* kUser = "user";

bool IsPluginFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

}  // namespace

PluginConfig ParsePluginConfig(const std::string& json_text) {
    PluginConfig config;
    if (json_text.empty()) return config;

    const auto parsed = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return config;

    if (auto it = parsed.find("plugins"); it != parsed.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            PluginConfig::Entry entry;
            entry.file = item.value("file", std::string{});
            if (entry.file.empty()) continue;
            entry.provenance =
                item.value("provenance", std::string{kUser}) == kBundled ? Provenance::Bundled
                                                                         : Provenance::User;
            entry.enabled = item.value("enabled", false);
            if (auto config_it = item.find("config"); config_it != item.end()) {
                if (config_it->is_object()) {
                    entry.config = *config_it;
                } else {
                    // Degrade-don't-throw, like the rest of this file: a
                    // malformed config must not stop the plugin from loading.
                    // The plugin sees {} and the warning says what was dropped.
                    common::DefaultLogger().warn(
                        "plugins.json: entry '{}' has a non-object 'config' ({}); "
                        "ignoring it -- the plugin will receive an empty config",
                        entry.file, config_it->type_name());
                }
            }
            config.plugins.push_back(std::move(entry));
        }
    }

    if (auto it = parsed.find("command_aliases"); it != parsed.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) config.command_aliases.emplace(key, value.get<std::string>());
        }
    }
    return config;
}

std::string SerializePluginConfig(const PluginConfig& config) {
    nlohmann::json out;
    out["plugins"] = nlohmann::json::array();
    for (const auto& entry : config.plugins) {
        nlohmann::json item = {
            {"file", entry.file},
            {"provenance", entry.provenance == Provenance::Bundled ? kBundled : kUser},
            {"enabled", entry.enabled},
        };
        // User config must survive the load->save cycle: main rewrites this file
        // on every startup, so dropping config here would silently erase it.
        // Emitted unconditionally (object or not) so what round-trips is what
        // was parsed -- a malformed value was already flattened to {} at parse.
        item["config"] = entry.config;
        out["plugins"].push_back(std::move(item));
    }
    out["command_aliases"] = nlohmann::json(config.command_aliases);
    return out.dump(2) + "\n";
}

// Kept in this namespace because that is where callers look for it; the layout
// probe itself is shared with the bundled prompt dir in minimal::common.
std::filesystem::path BundledPluginDir() { return common::BundledPluginDir(); }

std::vector<PluginCandidate> DiscoverPlugins(const std::filesystem::path& bundled_dir,
                                              const std::filesystem::path& user_dir,
                                              PluginConfig& config) {
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

    // Recurse for bundled so the build tree (one subdirectory per plugin) works
    // the same as the flat installed layout.
    scan(bundled_dir, Provenance::Bundled, /*recurse=*/true);
    scan(user_dir, Provenance::User, /*recurse=*/false);

    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.provenance != b.provenance) return a.provenance == Provenance::Bundled;
        return a.file < b.file;
    });

    std::vector<PluginCandidate> candidates;
    candidates.reserve(found.size());

    for (const auto& item : found) {
        auto existing = std::find_if(config.plugins.begin(), config.plugins.end(),
                                     [&](const PluginConfig::Entry& e) { return e.file == item.file; });

        if (existing == config.plugins.end()) {
            // Provenance decides the default, mechanically: shipped-with-this-build
            // is trusted, dropped-in-later is opt-in.
            PluginConfig::Entry entry;
            entry.file = item.file;
            entry.provenance = item.provenance;
            entry.enabled = (item.provenance == Provenance::Bundled);
            config.plugins.push_back(entry);
            existing = std::prev(config.plugins.end());
        } else if (existing->provenance != item.provenance) {
            // The same basename discovered from BOTH directories: enablement is
            // keyed by bare filename, so the two share one entry -- and both .so
            // files still load, which is a guaranteed fatal name collision the
            // user cannot see coming from plugins.json. Say so at discovery time;
            // the documented replacement flow is disabling the bundled entry, and
            // that needs the two to be distinguishable on disk at least.
            common::DefaultLogger().warn(
                "plugin '{}' exists both bundled and in the user plugin dir; both will load and "
                "the names will collide -- disable one of the two in plugins.json",
                item.file);
        }

        candidates.push_back(
            PluginCandidate{item.path, item.provenance, existing->enabled, existing->config});
    }

    return candidates;
}

}  // namespace minimal::plugins
