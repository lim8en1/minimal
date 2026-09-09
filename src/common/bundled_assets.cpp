#include "minimal/common/bundled_assets.hpp"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <vector>

#include "minimal/common/logger.hpp"

namespace minimal::common {

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
    // No supported way to ask. Said out loud rather than returned as an empty
    // path, because the symptom downstream is a server that starts cleanly with
    // no tools and no provider -- which reads like a config problem.
    DefaultLogger().error(
        "cannot locate the running executable on this platform; bundled plugins will not be "
        "found. Only macOS and Linux are supported (ARCHITECTURE.md, Deferred).");
    return {};
#endif
}

std::filesystem::path BundledPluginDir() {
    // MINIMAL_PLUGIN_DIR is the same value CMake installs to, so overriding
    // MINIMAL_PLUGIN_INSTALL_DIR moves both halves together. It used to be
    // hardcoded here, which meant the two could silently disagree.
    //
    // Installed first, then the build tree. Build tree: each plugin lands in its
    // own subdirectory under build/plugins/server, so this returns the parent and
    // discovery scans one level deeper -- it handles both shapes.
    //
    // Returning the installed path on a miss rather than empty is deliberate:
    // callers treat a nonexistent directory as "nothing bundled", and a real path
    // in a log is more useful than "".
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bin_dir = exe.parent_path();

    std::error_code ec;
    const auto installed = bin_dir.parent_path() / MINIMAL_PLUGIN_DIR;
    if (std::filesystem::is_directory(installed, ec)) return installed;

    const auto staged = bin_dir.parent_path().parent_path() / "plugins/server";
    if (std::filesystem::is_directory(staged, ec)) return staged;

    return installed;
}

std::filesystem::path BundledClientPluginDir() {
    // The client twin of the probe above; MINIMAL_CLIENT_PLUGIN_DIR is baked
    // in as a PRIVATE compile definition on minimal_common itself
    // (src/common/CMakeLists.txt, fed from the MINIMAL_CLIENT_PLUGIN_INSTALL_DIR
    // cache variable in the root CMakeLists.txt), not by the client's own
    // CMakeLists.txt -- so overriding that cache variable moves both halves
    // together regardless of which target is being built.
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bin_dir = exe.parent_path();

    std::error_code ec;
    const auto installed = bin_dir.parent_path() / MINIMAL_CLIENT_PLUGIN_DIR;
    if (std::filesystem::is_directory(installed, ec)) return installed;

    // Build tree: the client binary sits at <build>/src/client, so TWO hops
    // reach <build>/plugins/client -- the same shape the server probe uses
    // (its binary sits at <build>/src/server). Three hops used to reach the
    // repository ROOT's plugins/client -- the source directory, never the
    // built artifacts -- which nothing noticed until the first bundled client
    // plugin existed, because the fixtures load .so paths directly.
    const auto staged = bin_dir.parent_path().parent_path() / "plugins/client";
    if (std::filesystem::is_directory(staged, ec)) return staged;

    return installed;
}

}  // namespace minimal::common
