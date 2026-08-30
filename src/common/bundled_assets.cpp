#include "minimal/common/bundled_assets.hpp"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <vector>

namespace minimal::common {
namespace {

// The installed-then-build-tree probe both bundled dirs need. `installed` is
// relative to the install prefix (the executable's parent's parent), `staged` to
// the build root. Returning `installed` on a miss rather than empty is
// deliberate: callers treat a nonexistent directory as "nothing bundled", and a
// real path in a log is more useful than "".
std::filesystem::path ProbeLayouts(const std::filesystem::path& installed_relative,
                                   const std::filesystem::path& staged_relative) {
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bin_dir = exe.parent_path();

    std::error_code ec;
    const auto installed = bin_dir.parent_path() / installed_relative;
    if (std::filesystem::is_directory(installed, ec)) return installed;

    const auto staged = bin_dir.parent_path().parent_path() / staged_relative;
    if (std::filesystem::is_directory(staged, ec)) return staged;

    return installed;
}

}  // namespace

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

std::filesystem::path BundledPluginDir() {
    // MINIMAL_PLUGIN_DIR is the same value CMake installs to, so overriding
    // MINIMAL_PLUGIN_INSTALL_DIR moves both halves together. It used to be
    // hardcoded here, which meant the two could silently disagree.
    //
    // Build tree: each plugin lands in its own subdirectory under
    // build/plugins/server, so this returns the parent and discovery scans one
    // level deeper -- it handles both shapes.
    return ProbeLayouts(MINIMAL_PLUGIN_DIR, "plugins/server");
}

std::filesystem::path BundledPromptDir() {
    return ProbeLayouts(MINIMAL_PROMPT_DIR, "share/minimal/prompts");
}

std::vector<std::string> SeedBundledPrompts(const std::filesystem::path& bundled_dir,
                                            const std::filesystem::path& config_dir) {
    std::vector<std::string> seeded;
    std::error_code ec;
    if (bundled_dir.empty() || !std::filesystem::is_directory(bundled_dir, ec)) return seeded;

    const auto target_dir = config_dir / "prompts";
    for (const auto& entry : std::filesystem::directory_iterator(bundled_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const auto target = target_dir / entry.path().filename();
        if (std::filesystem::exists(target, ec)) continue;

        std::filesystem::create_directories(target_dir, ec);
        std::error_code copy_ec;
        std::filesystem::copy_file(entry.path(), target, copy_ec);
        if (!copy_ec) seeded.push_back(entry.path().filename().string());
    }
    return seeded;
}

}  // namespace minimal::common
