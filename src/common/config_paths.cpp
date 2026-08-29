#include "minimal/common/config_paths.hpp"

#include <cstdlib>
#include <stdexcept>

namespace minimal::common {
namespace {

// getenv, but treats an empty value as unset -- an exported-but-empty XDG_CONFIG_HOME
// is common and must not resolve to "/minimal".
const char* EnvOrNull(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

std::filesystem::path HomeOrThrow() {
    if (const char* home = EnvOrNull("HOME")) return home;
    throw std::runtime_error("cannot resolve config directory: $HOME is unset");
}

}  // namespace

std::filesystem::path ConfigDir() {
    if (const char* override_dir = EnvOrNull("MINIMAL_CONFIG_DIR")) return override_dir;
    if (const char* xdg = EnvOrNull("XDG_CONFIG_HOME")) return std::filesystem::path(xdg) / "minimal";
    return HomeOrThrow() / ".config" / "minimal";
}

std::filesystem::path DataDir() {
    if (const char* xdg = EnvOrNull("XDG_DATA_HOME")) return std::filesystem::path(xdg) / "minimal";
    return HomeOrThrow() / ".local" / "share" / "minimal";
}

}  // namespace minimal::common
