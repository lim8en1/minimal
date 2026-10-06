#include "minimal/common/plugin_config.hpp"

#include <algorithm>
#include <iterator>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::common {
namespace {

constexpr const char* kBundled = "bundled";
constexpr const char* kUser = "user";

bool IsPluginFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

// One plugin's config file: {} when it does not exist (or is empty), the
// object when it parses, an error otherwise.
void LoadPluginConfig(const PluginConfigFormat& format, const std::filesystem::path& module,
                      PluginFileCandidate& candidate) {
    if (format.config_dir.empty()) return;
    const std::string stem = module.stem().string();
    const std::string label = PluginConfigLabel(format.config_dir_label, stem);
    try {
        nlohmann::json loaded = config::LoadJsonConfig<nlohmann::json>(
            format.config_dir / (stem + ".json"), label, config::AbsencePolicy::kDefaultOnMissing,
            config::EmptyFilePolicy::kTreatAsAbsent, [&](const nlohmann::json& doc) {
                if (!doc.is_object()) {
                    throw config::ConfigError(label + ": must be a JSON object");
                }
                return doc;
            });
        if (loaded.is_object()) candidate.config = std::move(loaded);  // absent: null -> {}
    } catch (const config::ConfigError& error) {
        candidate.config_error = error.what();
    }
}

}  // namespace

PluginConfigFile ParsePluginConfigFile(const std::string& json_text,
                                       const PluginConfigFormat& format) {
    PluginConfigFile config;
    if (json_text.empty()) return config;

    const auto parsed = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        config.malformed = true;  // the host reports it and skips the rewrite
        return config;
    }
    const std::string& label = format.file_label;
    const auto problem = [&](const std::exception& error) {
        config.problems.emplace_back(error.what());
    };

    if (const auto it = parsed.find("plugins"); it != parsed.end()) {
        if (!it->is_array()) {
            config.problems.push_back(label + ": plugins: must be a list of plugin entries");
        } else {
            for (std::size_t index = 0; index < it->size(); ++index) {
                const auto& item = (*it)[index];
                const std::string segment = "plugins[" + std::to_string(index) + "]";
                try {
                    if (!item.is_object()) {
                        throw config::ConfigError(label + ": " + segment + ": must be an object");
                    }
                    config::Reader reader(item, label, segment);
                    PluginConfigFile::Entry entry;
                    entry.file = reader.RequireString("file");
                    if (const auto provenance = reader.MaybeEnum("provenance", {kBundled, kUser})) {
                        entry.provenance = *provenance == kBundled ? PluginProvenance::Bundled
                                                                   : PluginProvenance::User;
                    }
                    entry.enabled = reader.MaybeBool("enabled");
                    if (reader.Has("config")) {
                        const auto stem = std::filesystem::path(entry.file).stem().string();
                        reader.Fail("config", "per-plugin config moved to its own file, " +
                                                  PluginConfigLabel(format.config_dir_label, stem) +
                                                  " -- put this object there");
                    }
                    reader.Finish();
                    config.plugins.push_back(std::move(entry));
                } catch (const config::ConfigError& error) {
                    problem(error);
                }
            }
        }
    }

    if (const auto it = parsed.find("aliases"); it != parsed.end()) {
        try {
            config::Reader root(parsed, label);
            if (auto aliases = root.MaybeStringMap("aliases")) {
                config.aliases = {aliases->begin(), aliases->end()};
            }
        } catch (const config::ConfigError& error) {
            problem(error);
        }
    }

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
        nlohmann::json item = {{"file", entry.file}};
        // Written once known: a hand-written entry that omitted them gets the
        // resolved values persisted, so the next edit starts from the truth.
        if (entry.provenance) {
            item["provenance"] = *entry.provenance == PluginProvenance::Bundled ? kBundled : kUser;
        }
        if (entry.enabled) item["enabled"] = *entry.enabled;
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
        // An entry that names no provenance claims the file by name -- unless
        // both directories ship that name, where guessing could enable a
        // plugin nobody opted into: that is a named problem instead.
        if (existing == config.plugins.end()) {
            existing = std::find_if(config.plugins.begin(), config.plugins.end(),
                                    [&](const PluginConfigFile::Entry& e) {
                                        return e.file == item.file && !e.provenance;
                                    });
            if (existing != config.plugins.end()) {
                const bool ambiguous = std::any_of(found.begin(), found.end(), [&](const Found& e) {
                    return e.file == item.file && e.provenance != item.provenance;
                });
                if (ambiguous) {
                    // Reported once, from the bundled side (sorted first).
                    if (item.provenance == PluginProvenance::Bundled) config.problems.push_back(
                        format.file_label + ": the entry for '" + item.file +
                        "' names no provenance, and both a bundled and a user plugin of that "
                        "name exist -- add \"provenance\": \"bundled\" or \"user\"");
                    existing = config.plugins.end();
                    continue;
                }
                existing->provenance = item.provenance;
            }
        }

        // Provenance decides the default, mechanically: shipped-with-this-build
        // is trusted, dropped-in-later is opt-in -- except a bundled plugin
        // whose build marked it disabled-by-default (kDisabledByDefaultSuffix).
        // Once an entry says `enabled`, the user's choice governs and this
        // default is never consulted again.
        std::error_code marker_ec;
        const auto marker =
            item.path.parent_path() / (item.path.stem().string() + kDisabledByDefaultSuffix);
        const bool default_enabled = item.provenance == PluginProvenance::Bundled &&
                                     !std::filesystem::exists(marker, marker_ec);
        if (existing == config.plugins.end()) {
            PluginConfigFile::Entry entry;
            entry.file = item.file;
            entry.provenance = item.provenance;
            config.plugins.push_back(entry);
            existing = std::prev(config.plugins.end());
        }
        if (!existing->enabled) existing->enabled = default_enabled;

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

        PluginFileCandidate candidate{item.path, item.provenance, *existing->enabled};
        // Only for a plugin that will load: a disabled plugin's file is not
        // its host's business, and a broken one must not fail a plugin the
        // user switched off.
        if (candidate.enabled) LoadPluginConfig(format, item.path, candidate);
        candidates.push_back(std::move(candidate));
    }

    return candidates;
}

}  // namespace minimal::common
