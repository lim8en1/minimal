#include "minimal/common/plugin_config.hpp"

#include <algorithm>
#include <iterator>

#include "minimal/common/logger.hpp"

namespace minimal::common {
namespace {

constexpr const char* kBundled = "bundled";
constexpr const char* kUser = "user";

bool IsPluginFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

}  // namespace

PluginConfigFile ParsePluginConfigFile(const std::string& json_text,
                                       const PluginConfigFormat& format) {
    PluginConfigFile config;
    if (json_text.empty()) return config;

    const auto parsed = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        config.malformed = true;  // the host warns and skips the rewrite
        return config;
    }
    const std::string& label = format.file_label;

    if (auto it = parsed.find("plugins"); it != parsed.end() && !it->is_array()) {
        common::DefaultLogger().warn(
            "{}: 'plugins' is a {}, not an array; ignoring it -- no plugins will load", label,
            it->type_name());
    }
    if (auto it = parsed.find("plugins"); it != parsed.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            PluginConfigFile::Entry entry;
            // Type-checked reads, not item.value(key, default): that overload only
            // substitutes the default when the key is ABSENT -- a key present with
            // the wrong JSON type (a user hand-editing this file can write
            // "enabled": "false" or "enabled": 1) makes it throw type_error.302
            // uncaught, which discards the ENTIRE parsed config at the caller (or
            // aborts startup). Degrade-don't-throw: say what was dropped.
            if (auto file_it = item.find("file"); file_it != item.end()) {
                if (file_it->is_string()) {
                    entry.file = file_it->get<std::string>();
                } else {
                    common::DefaultLogger().warn(
                        "{}: a plugin entry has a non-string 'file' ({}); skipping it", label,
                        file_it->type_name());
                }
            }
            if (entry.file.empty()) continue;
            entry.provenance = PluginProvenance::User;
            if (auto prov_it = item.find("provenance");
                prov_it != item.end() && prov_it->is_string()) {
                entry.provenance = prov_it->get<std::string>() == kBundled
                                       ? PluginProvenance::Bundled
                                       : PluginProvenance::User;
            }
            entry.enabled = false;
            if (auto enabled_it = item.find("enabled"); enabled_it != item.end()) {
                if (enabled_it->is_boolean()) {
                    entry.enabled = enabled_it->get<bool>();
                } else {
                    common::DefaultLogger().warn(
                        "{}: entry '{}' has a non-boolean 'enabled' ({}); defaulting to "
                        "disabled",
                        label, entry.file, enabled_it->type_name());
                }
            }
            if (auto config_it = item.find("config"); config_it != item.end()) {
                if (config_it->is_object()) {
                    entry.config = *config_it;
                } else {
                    // Degrade-don't-throw: a malformed config must not stop the
                    // plugin from loading. The plugin sees {} and the warning says
                    // what was dropped.
                    common::DefaultLogger().warn(
                        "{}: entry '{}' has a non-object 'config' ({}); ignoring it -- the "
                        "plugin will receive an empty config",
                        label, entry.file, config_it->type_name());
                }
            }
            config.plugins.push_back(std::move(entry));
        }
    }

    if (auto it = parsed.find("aliases"); it != parsed.end() && !it->is_object()) {
        common::DefaultLogger().warn("{}: 'aliases' is a {}, not an object; ignoring it", label,
                                     it->type_name());
    }
    if (auto it = parsed.find("aliases"); it != parsed.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) config.aliases.emplace(key, value.get<std::string>());
        }
    }
    if (format.check_keys) format.check_keys(parsed);

    // Anything this parser does not model is kept verbatim, so serializing back
    // does not silently delete it. See PluginConfigFile::preserved.
    for (const auto& [key, value] : parsed.items()) {
        if (key == "plugins" || key == "aliases") continue;
        config.preserved[key] = value;
    }
    return config;
}

std::string SerializePluginConfigFile(const PluginConfigFile& config) {
    // Starts from what was read, so unmodelled keys survive the round trip.
    nlohmann::json out = config.preserved;
    out["plugins"] = nlohmann::json::array();
    for (const auto& entry : config.plugins) {
        nlohmann::json item = {
            {"file", entry.file},
            {"provenance", entry.provenance == PluginProvenance::Bundled ? kBundled : kUser},
            {"enabled", entry.enabled},
        };
        // User config must survive the load->save cycle: the file is rewritten on
        // every startup, so dropping config here would silently erase it. Emitted
        // unconditionally (object or not) so what round-trips is what was parsed
        // -- a malformed value was already flattened to {} at parse.
        item["config"] = entry.config;
        out["plugins"].push_back(std::move(item));
    }
    // Only when non-empty: an always-present empty key is noise in a file the
    // user is invited to edit, and would resurrect "aliases": {} on the very
    // boot after an operator removed their last one.
    if (!config.aliases.empty()) out["aliases"] = nlohmann::json(config.aliases);
    return out.dump(2) + "\n";
}

std::vector<PluginFileCandidate> DiscoverPluginFiles(const std::filesystem::path& bundled_dir,
                                                     const std::filesystem::path& user_dir,
                                                     PluginConfigFile& config,
                                                     const PluginConfigFormat& format) {
    struct Found {
        std::filesystem::path path;
        std::string file;
        PluginProvenance provenance;
    };
    std::vector<Found> found;

    // Constructed with an error_code, but the range-based-for form still calls the
    // THROWING operator++ on every step -- a permission-denied or
    // concurrently-removed subdirectory mid-scan would throw filesystem_error
    // uncaught and crash startup. Manual iteration via the error_code-taking
    // `increment()` keeps every step non-throwing; a mid-scan error just ends that
    // scan early.
    auto scan = [&](const std::filesystem::path& dir, PluginProvenance provenance, bool recurse) {
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
        const auto collect = [&](auto it) {
            const decltype(it) end;
            for (; !ec && it != end; it.increment(ec)) {
                std::error_code file_ec;
                if (it->is_regular_file(file_ec) && IsPluginFile(it->path())) {
                    found.push_back({it->path(), it->path().filename().string(), provenance});
                }
            }
        };
        if (recurse) {
            collect(std::filesystem::recursive_directory_iterator(dir, ec));
        } else {
            collect(std::filesystem::directory_iterator(dir, ec));
        }
    };

    // Recurse for bundled so the build tree (one subdirectory per plugin) works the
    // same as the flat installed layout.
    scan(bundled_dir, PluginProvenance::Bundled, /*recurse=*/true);
    scan(user_dir, PluginProvenance::User, /*recurse=*/false);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.provenance != b.provenance) return a.provenance == PluginProvenance::Bundled;
        return a.file < b.file;
    });

    std::vector<PluginFileCandidate> candidates;
    candidates.reserve(found.size());

    for (const auto& item : found) {
        // Keyed by (provenance, file), NOT file alone: a user-dir plugin that
        // happens to share a basename with a bundled one must never match the
        // bundled entry and inherit its enabled:true -- that dlopened and ran a
        // plugin nobody opted into, with no plugins.json edit. Two independent
        // entries let the user toggle each side separately, which is also what the
        // collision warning below tells them to do.
        auto existing = std::find_if(config.plugins.begin(), config.plugins.end(),
                                     [&](const PluginConfigFile::Entry& e) {
                                         return e.file == item.file &&
                                                e.provenance == item.provenance;
                                     });

        if (existing == config.plugins.end()) {
            // Provenance decides the default, mechanically: shipped-with-this-build
            // is trusted, dropped-in-later is opt-in -- except a bundled plugin the
            // host lists as defaulting to disabled (it spends the user's tokens or
            // data on its own initiative, so enabling it is a deliberate act). Once
            // an entry exists, the user's `enabled` governs and this default is
            // never consulted again.
            PluginConfigFile::Entry entry;
            entry.file = item.file;
            entry.provenance = item.provenance;
            entry.enabled = item.provenance == PluginProvenance::Bundled &&
                            !(format.defaults_disabled && format.defaults_disabled(item.file));
            config.plugins.push_back(entry);
            existing = std::prev(config.plugins.end());
        }

        // The same basename discovered from BOTH directories: both files still load
        // (each under its own independent entry), which is a guaranteed name
        // collision the user cannot see coming from the config file alone. Scanned
        // over `found`, not config.plugins: the persisted list keeps entries for
        // files that have since been deleted, so a stale entry would warn forever
        // about a collision that no longer exists. `found` is sorted with both
        // provenances present, so each side of a real collision sees the other.
        const bool also_from_other_provenance =
            std::any_of(found.begin(), found.end(), [&](const Found& e) {
                return e.file == item.file && e.provenance != item.provenance;
            });
        if (also_from_other_provenance) {
            common::DefaultLogger().warn(
                "{} '{}' exists both bundled and in the user plugin dir; both will load and the "
                "names will collide -- disable one of the two in {}",
                format.noun, item.file, format.file_label);
        }

        // Same basename found TWICE within the SAME provenance -- realistic for the
        // recursive bundled scan (a build misconfiguration, or a copy-pasted plugin
        // subdirectory). Both files are still discovered and dlopen'd, but they
        // share ONE entry (matched by (provenance, file)), so the user cannot
        // disable one without the other; without this warning there is no
        // explanation for why toggling one entry affects two plugins.
        const bool also_same_provenance =
            std::any_of(found.begin(), found.end(), [&](const Found& e) {
                return e.path != item.path && e.file == item.file &&
                       e.provenance == item.provenance;
            });
        if (also_same_provenance) {
            common::DefaultLogger().warn(
                "{} '{}' was found more than once under the same provenance ({}) at {}; they "
                "share one {} entry and its enabled toggle -- rename one of the files to tell "
                "them apart",
                format.noun, item.file,
                item.provenance == PluginProvenance::Bundled ? kBundled : kUser,
                item.path.string(), format.file_label);
        }

        candidates.push_back(PluginFileCandidate{item.path, item.provenance, existing->enabled,
                                                 existing->config});
    }

    return candidates;
}

}  // namespace minimal::common
