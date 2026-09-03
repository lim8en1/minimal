#include "minimal/common/config_paths.hpp"

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

}  // namespace

std::filesystem::path ConfigDir() {
    if (auto override_dir = LookupEnv("MINIMAL_CONFIG_DIR")) return *override_dir;
    if (auto xdg = LookupEnv("XDG_CONFIG_HOME")) return std::filesystem::path(*xdg) / "minimal";
    return HomeOrThrow() / ".config" / "minimal";
}

std::filesystem::path DataDir() {
    if (auto xdg = LookupEnv("XDG_DATA_HOME")) return std::filesystem::path(*xdg) / "minimal";
    return HomeOrThrow() / ".local" / "share" / "minimal";
}

}  // namespace minimal::common
