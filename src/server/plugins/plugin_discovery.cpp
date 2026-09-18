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

// Bundled plugins that default to DISABLED despite their provenance. Everything
// else bundled is load-bearing for the default harness (tools, provider,
// commands) and ships enabled. The list exists because session-titler is the
// first bundled plugin whose entire job is to spend the user's tokens on its
// own initiative (~100 per session) -- trusted code, but a cost the operator
// should opt into. Match by basename, the same key plugins.json entries use.
bool DefaultsDisabled(std::string_view file) {
    // session-titler: spends the user's tokens on its own initiative.
    // memory: writes to disk on the model's initiative and (under the http
    // embedding strategy) sends entry text to an external endpoint; enabling
    // it is the operator's informed decision. typesafe-advisor: sends tool
    // input to an external endpoint AND auto-answers permission prompts on the
    // model's behalf -- the operator's two informed decisions, not defaults.
    // All ship present but disabled; once an entry exists in plugins.json the
    // user's `enabled` governs and this list is never consulted again.
    return file == "session-titler.so" || file == "session-titler.dylib" ||
           file == "memory.so" || file == "memory.dylib" ||
           file == "typesafe-advisor.so" || file == "typesafe-advisor.dylib";
}

}  // namespace

PluginConfig ParsePluginConfig(const std::string& json_text) {
    PluginConfig config;
    if (json_text.empty()) return config;

    const auto parsed = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        config.malformed = true;  // the caller warns and skips the rewrite
        return config;
    }

    if (auto it = parsed.find("plugins"); it != parsed.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            PluginConfig::Entry entry;
            // Type-checked reads, not item.value(key, default): that overload
            // only substitutes the default when the key is ABSENT -- a key
            // present with the wrong JSON type (a user hand-editing this file
            // can write "enabled": "false" or "enabled": 1) makes it throw
            // type_error.302 uncaught, which propagates out of this function
            // (it has no try/catch of its own) and discards the ENTIRE parsed
            // config at the caller, silently re-deriving every plugin's
            // enabled state from its provenance default. Degrade-don't-throw,
            // like the rest of this file (see 'config' below) -- ported from
            // the client's identical fix in
            // src/client/plugins/plugin_discovery.cpp.
            if (auto file_it = item.find("file"); file_it != item.end()) {
                if (file_it->is_string()) {
                    entry.file = file_it->get<std::string>();
                } else {
                    common::DefaultLogger().warn(
                        "plugins.json: a plugin entry has a non-string 'file' ({}); skipping it",
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
                        "plugins.json: entry '{}' has a non-boolean 'enabled' ({}); defaulting "
                        "to disabled",
                        entry.file, enabled_it->type_name());
                }
            }
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

    if (auto it = parsed.find("aliases"); it != parsed.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) config.aliases.emplace(key, value.get<std::string>());
        }
    }
    // No legacy fallback: the old key is IGNORED (it round-trips via
    // `preserved`, so it is not deleted either). Silence would mean aliases
    // quietly not applying after an upgrade, so say so on every boot -- the
    // noise is the feature.
    if (parsed.contains("command_aliases")) {
        common::DefaultLogger().warn(
            "plugins.json: \"command_aliases\" was renamed to \"aliases\" and no longer "
            "applies; rename the key");
    }

    // Anything this parser does not model is kept verbatim, so serializing back
    // does not silently delete it. See PluginConfig::preserved.
    for (const auto& [key, value] : parsed.items()) {
        if (key == "plugins" || key == "aliases") continue;
        config.preserved[key] = value;
    }
    return config;
}

std::string SerializePluginConfig(const PluginConfig& config) {
    // Starts from what was read, so unmodelled keys survive the round trip.
    nlohmann::json out = config.preserved;
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
    // Only when non-empty: an always-present empty key is noise in a file the
    // user is invited to edit, and would resurrect "aliases": {} on the very
    // boot after an operator removed their last one.
    if (!config.aliases.empty()) out["aliases"] = nlohmann::json(config.aliases);
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
        // Keyed by (provenance, file), NOT file alone: a user-dir plugin that
        // happens to share a basename with a bundled one must never match the
        // bundled entry and inherit its enabled:true. Before this, a dropped-in
        // ~/.config/minimal/plugins/tools-fs.so matched the bundled tools-fs
        // entry, was discovered enabled, dlopened, and ran its static
        // initializers and every registration hook -- with no plugins.json
        // edit and no confirmation -- before the (still-real) name collision
        // aborted startup. Two independent entries let the user toggle each
        // side separately, which is also what the collision warning below now
        // tells them to do.
        auto existing = std::find_if(config.plugins.begin(), config.plugins.end(),
                                     [&](const PluginConfig::Entry& e) {
                                         return e.file == item.file &&
                                                e.provenance == item.provenance;
                                     });

        if (existing == config.plugins.end()) {
            // Provenance decides the default, mechanically: shipped-with-this-build
            // is trusted, dropped-in-later is opt-in. One exception, listed below:
            // a bundled plugin that spends the user's tokens on its own
            // initiative is shipped present but disabled, so enabling it is a
            // deliberate act. Once an entry exists in plugins.json, the user's
            // `enabled` governs and this default is never consulted again.
            PluginConfig::Entry entry;
            entry.file = item.file;
            entry.provenance = item.provenance;
            entry.enabled = (item.provenance == Provenance::Bundled) &&
                            !DefaultsDisabled(item.file);
            config.plugins.push_back(entry);
            existing = std::prev(config.plugins.end());
        }

        // The same basename discovered from BOTH directories: both .so files
        // still load (each under its own now-independent entry), which is a
        // guaranteed fatal name collision the user cannot see coming from
        // plugins.json alone. Say so at discovery time; the documented
        // replacement flow is disabling one of the two entries.
        //
        // Scanned over `found`, not config.plugins: the persisted list keeps
        // entries for files that have since been deleted (it is never pruned),
        // so a stale entry would warn forever about a collision that no longer
        // exists -- and `found` is sorted with both provenances present, so
        // each side of a real collision sees the other regardless of loop
        // order.
        const bool also_from_other_provenance = std::any_of(
            found.begin(), found.end(), [&](const Found& e) {
                return e.file == item.file && e.provenance != item.provenance;
            });
        if (also_from_other_provenance) {
            common::DefaultLogger().warn(
                "plugin '{}' exists both bundled and in the user plugin dir; both will load and "
                "the names will collide -- disable one of the two in plugins.json",
                item.file);
        }

        // Same basename found TWICE within the SAME provenance -- realistic
        // for the recursive bundled scan (a build misconfiguration, or a
        // copy-pasted plugin subdirectory). Both distinct files are still
        // discovered and dlopen'd independently (the loop below pushes one
        // PluginCandidate per `found` entry), but they are keyed to the SAME
        // plugins.json entry above (matched by (provenance, file) alone), so
        // the user cannot disable one without disabling both -- and without
        // this warning, gets no explanation for why toggling one entry
        // affects two unrelated plugins.
        const bool also_same_provenance = std::any_of(
            found.begin(), found.end(), [&](const Found& e) {
                return e.path != item.path && e.file == item.file &&
                       e.provenance == item.provenance;
            });
        if (also_same_provenance) {
            common::DefaultLogger().warn(
                "plugin '{}' was found more than once under the same provenance ({}) at {}; they "
                "share one plugins.json entry and its enabled toggle -- rename one of the files "
                "to tell them apart",
                item.file, item.provenance == Provenance::Bundled ? kBundled : kUser,
                item.path.string());
        }

        candidates.push_back(
            PluginCandidate{item.path, item.provenance, existing->enabled, existing->config});
    }

    return candidates;
}

}  // namespace minimal::plugins
