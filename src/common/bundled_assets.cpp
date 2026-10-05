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

namespace {

// Installed tree first (<prefix>/bin/<exe> -> <prefix>/<install_rel>), then the
// build tree (<build>/src/<host>/<exe> -> <build>/<staged_rel>; each plugin
// sits in its own subdirectory there and discovery recurses). `install_rel` is
// the same value CMake installs to, baked in as a compile definition, so
// overriding the install dir moves the search with it.
//
// A miss returns the installed path rather than empty: callers treat a missing
// directory as "nothing bundled", and a real path in a log beats "".
std::filesystem::path ProbePluginDir(const char* install_rel, const char* staged_rel) {
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bin_dir = exe.parent_path();

    std::error_code ec;
    const auto installed = bin_dir.parent_path() / install_rel;
    if (std::filesystem::is_directory(installed, ec)) return installed;

    const auto staged = bin_dir.parent_path().parent_path() / staged_rel;
    if (std::filesystem::is_directory(staged, ec)) return staged;

    return installed;
}

}  // namespace

std::filesystem::path BundledPluginDir() {
    return ProbePluginDir(MINIMAL_PLUGIN_DIR, "plugins/server");
}

std::filesystem::path BundledClientPluginDir() {
    return ProbePluginDir(MINIMAL_CLIENT_PLUGIN_DIR, "plugins/client");
}

}  // namespace minimal::common
