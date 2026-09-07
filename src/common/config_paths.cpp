#include "minimal/common/config_paths.hpp"

#include "minimal/common/logger.hpp"

#include <cstdlib>
#include <stdexcept>

namespace minimal::common {

std::optional<std::string> LookupEnv(std::string_view name) {
    const char* v = std::getenv(std::string(name).c_str());
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

namespace {

std::filesystem::path HomeOrThrow() {
    if (auto home = LookupEnv("HOME")) return *home;
    throw std::runtime_error("cannot resolve config directory: $HOME is unset");
}

// XDG requires these variables to be ABSOLUTE. A relative value used to be
// accepted verbatim -- and a spawned server chdir's to "/", so
// XDG_CONFIG_HOME=relpath silently resolved under / there while the client
// resolved it against its own cwd: two processes, two different
// directories. Rejected (with the default standing in), like an unset var.
std::filesystem::path XdgDirOr(const char* env_name, std::filesystem::path fallback) {
    if (auto xdg = LookupEnv(env_name)) {
        std::filesystem::path dir(*xdg);
        if (dir.is_absolute()) return dir;
        common::DefaultLogger().warn(
            "{}='{}' is relative; the XDG spec requires an absolute path -- ignored, "
            "using the default location",
            env_name, *xdg);
    }
    return fallback;
}

}  // namespace

std::filesystem::path ConfigDir() {
    if (auto override_dir = LookupEnv("MINIMAL_CONFIG_DIR")) return *override_dir;
    return XdgDirOr("XDG_CONFIG_HOME", HomeOrThrow() / ".config") / "minimal";
}

std::filesystem::path DataDir() {
    // MINIMAL_DATA_DIR: the same explicit escape hatch MINIMAL_CONFIG_DIR is
    // for config -- an isolated runtime state dir (sessions, logs) without
    // touching XDG_DATA_HOME for everything else on the machine.
    if (auto override_dir = LookupEnv("MINIMAL_DATA_DIR")) return *override_dir;
    return XdgDirOr("XDG_DATA_HOME", HomeOrThrow() / ".local" / "share") / "minimal";
}

}  // namespace minimal::common
