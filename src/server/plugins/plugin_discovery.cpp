#include "minimal/plugins/plugin_discovery.hpp"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace minimal::plugins {
namespace {

constexpr const char* kBundled = "bundled";
constexpr const char* kUser = "user";

bool IsPluginFile(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

std::filesystem::path ExecutablePath() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
    return ec ? std::filesystem::path(buffer.data()) : resolved;
#elif defined(__linux__)
    std::error_code ec;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : resolved;
#else
    return {};
#endif
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
        out["plugins"].push_back({
            {"file", entry.file},
            {"provenance", entry.provenance == Provenance::Bundled ? kBundled : kUser},
            {"enabled", entry.enabled},
        });
    }
    out["command_aliases"] = nlohmann::json(config.command_aliases);
    return out.dump(2) + "\n";
}

std::filesystem::path BundledPluginDir() {
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bin_dir = exe.parent_path();

    // Installed layout: <prefix>/bin/minimal-server -> <prefix>/lib/minimal/plugins
    const auto installed = bin_dir.parent_path() / "lib" / "minimal" / "plugins";
    std::error_code ec;
    if (std::filesystem::is_directory(installed, ec)) return installed;

    // Build-tree layout: build/src/server/minimal-server -> build/plugins/server/*/
    // Each plugin lands in its own subdirectory there, so the caller scans one
    // level deeper; returning the parent lets DiscoverPlugins handle both shapes.
    const auto build_tree = bin_dir.parent_path().parent_path() / "plugins" / "server";
    if (std::filesystem::is_directory(build_tree, ec)) return build_tree;

    return installed;  // may not exist; discovery treats that as "no bundled plugins"
}

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
        }

        candidates.push_back(PluginCandidate{item.path, item.provenance, existing->enabled});
    }

    return candidates;
}

}  // namespace minimal::plugins
