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

// A directory named by an env var must be ABSOLUTE, whichever var it is. A
// relative value used to be accepted verbatim -- and a spawned server chdir's
// to "/", so XDG_CONFIG_HOME=relpath silently resolved under / there while the
// client resolved it against its own cwd: two processes, two different
// directories. Rejected (with the next candidate standing in), like an unset
// var. `reason` names why absoluteness is required, since XDG has a spec to
// cite and our own escape hatches do not.
std::optional<std::filesystem::path> AbsoluteOrWarn(const char* env_name, const char* reason) {
    auto value = LookupEnv(env_name);
    if (!value) return std::nullopt;
    std::filesystem::path dir(*value);
    if (dir.is_absolute()) return dir;
    common::DefaultLogger().warn("{}='{}' is relative; {} -- ignored, using the default location",
                                 env_name, *value, reason);
    return std::nullopt;
}

// The XDG variable if it answers, else $HOME/<suffix>.
//
// The fallback is a SUFFIX rather than a ready-made path because building it
// eagerly at the call site meant HomeOrThrow() ran unconditionally: an absolute
// XDG_CONFIG_HOME with $HOME unset (a systemd unit with no User=, a scratch
// container, a test that clears HOME) threw "$HOME is unset" even though XDG had
// fully answered the question -- and main() calls ConfigDir() outside any try.
std::filesystem::path XdgDirOr(const char* env_name, const std::filesystem::path& home_suffix) {
    if (auto dir = AbsoluteOrWarn(env_name, "the XDG spec requires an absolute path")) return *dir;
    return HomeOrThrow() / home_suffix;
}

// The MINIMAL_* escape hatches get the same absolute-path rule as XDG, for the
// same reason: SpawnDetached execl's with the inherited environ after chdir("/")
// (src/client/server_spawn.cpp), so a relative override resolves under / in the
// server and under the cwd in the client -- divergent sessions.db, auth.json,
// plugin storage and logs, with a socket path that still connects (it is
// anchored with std::filesystem::absolute) so nothing looks wrong.
constexpr const char* kOverrideMustBeAbsolute =
    "an override is resolved by both the client and the server, which do not share a "
    "working directory";

}  // namespace

std::filesystem::path ConfigDir() {
    if (auto override_dir = AbsoluteOrWarn("MINIMAL_CONFIG_DIR", kOverrideMustBeAbsolute)) {
        return *override_dir;
    }
    return XdgDirOr("XDG_CONFIG_HOME", ".config") / "minimal";
}

std::filesystem::path DataDir() {
    // MINIMAL_DATA_DIR: the same explicit escape hatch MINIMAL_CONFIG_DIR is
    // for config -- an isolated runtime state dir (sessions, logs) without
    // touching XDG_DATA_HOME for everything else on the machine.
    if (auto override_dir = AbsoluteOrWarn("MINIMAL_DATA_DIR", kOverrideMustBeAbsolute)) {
        return *override_dir;
    }
    return XdgDirOr("XDG_DATA_HOME", std::filesystem::path(".local") / "share") / "minimal";
}

}  // namespace minimal::common
