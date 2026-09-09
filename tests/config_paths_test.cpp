#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "minimal/common/config_paths.hpp"

using minimal::common::ConfigDir;
using minimal::common::DataDir;

namespace {

// RAII env var override so tests don't leak state into each other.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};

}  // namespace

TEST_CASE("MINIMAL_CONFIG_DIR overrides everything", "[config_paths]") {
    ScopedEnv override_dir("MINIMAL_CONFIG_DIR", "/tmp/minimal-test-config");
    ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/should-be-ignored");
    CHECK(ConfigDir() == "/tmp/minimal-test-config");
}

TEST_CASE("XDG_CONFIG_HOME is used when no explicit override", "[config_paths]") {
    ScopedEnv override_dir("MINIMAL_CONFIG_DIR", nullptr);
    ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/xdg");
    CHECK(ConfigDir() == "/tmp/xdg/minimal");
}

TEST_CASE("an exported-but-empty env var counts as unset", "[config_paths]") {
    // A common footgun: `export XDG_CONFIG_HOME=` must not resolve to "/minimal".
    ScopedEnv override_dir("MINIMAL_CONFIG_DIR", nullptr);
    ScopedEnv xdg("XDG_CONFIG_HOME", "");
    ScopedEnv home("HOME", "/tmp/home");
    CHECK(ConfigDir() == "/tmp/home/.config/minimal");
}

TEST_CASE("data dir is separate from config dir", "[config_paths]") {
    // Mutable runtime state (SQLite store, logs) must not land under ~/.config.
    ScopedEnv xdg_data("XDG_DATA_HOME", nullptr);
    ScopedEnv home("HOME", "/tmp/home");
    CHECK(DataDir() == "/tmp/home/.local/share/minimal");
    CHECK(DataDir() != ConfigDir());
}

TEST_CASE("MINIMAL_DATA_DIR is the data-side escape hatch", "[config_paths][invariant]") {
    // The config side has MINIMAL_CONFIG_DIR; the data side (sessions, logs)
    // had no equivalent, so an isolated runtime dir meant touching
    // XDG_DATA_HOME for everything else on the machine.
    ScopedEnv data_dir("MINIMAL_DATA_DIR", "/tmp/minimal-test-data");
    ScopedEnv xdg_data("XDG_DATA_HOME", "/tmp/should-be-ignored");
    CHECK(DataDir() == "/tmp/minimal-test-data");
}

TEST_CASE("a RELATIVE XDG value is rejected, not resolved against the cwd",
          "[config_paths][invariant]") {
    // The XDG spec requires absolute values. The old code accepted them --
    // and a spawned server (chdir'd to /) resolved the same string under /,
    // giving client and server two different directories.
    ScopedEnv xdg_config("XDG_CONFIG_HOME", "relative/path");
    ScopedEnv xdg_data("XDG_DATA_HOME", "relative/data");
    ScopedEnv home("HOME", "/tmp/home");
    CHECK(ConfigDir() == "/tmp/home/.config/minimal");
    CHECK(DataDir() == "/tmp/home/.local/share/minimal");
}

TEST_CASE("a RELATIVE override is rejected too, the same as a relative XDG value",
          "[config_paths][invariant]") {
    // MINIMAL_CONFIG_DIR/MINIMAL_DATA_DIR are resolved by both the client and a
    // spawned server (chdir'd to /), so a relative override reproduces exactly
    // the two-directories problem the XDG guard above exists to prevent.
    ScopedEnv override_config("MINIMAL_CONFIG_DIR", "relative/config");
    ScopedEnv override_data("MINIMAL_DATA_DIR", "relative/data");
    ScopedEnv xdg_config("XDG_CONFIG_HOME", nullptr);
    ScopedEnv xdg_data("XDG_DATA_HOME", nullptr);
    ScopedEnv home("HOME", "/tmp/home");
    CHECK(ConfigDir() == "/tmp/home/.config/minimal");
    CHECK(DataDir() == "/tmp/home/.local/share/minimal");
}

TEST_CASE("HOME unset does not stop an absolute XDG_CONFIG_HOME from answering",
          "[config_paths][invariant]") {
    // The home-based fallback used to be built EAGERLY (HomeOrThrow() / ".config"
    // computed as XdgDirOr's argument, before XdgDirOr got a chance to look at
    // the env var), so a $HOME-less environment (a systemd unit with no User=,
    // a scratch container) threw "$HOME is unset" even when XDG_CONFIG_HOME
    // fully answered the question. main() calls ConfigDir() with no try/catch,
    // so this used to be an uncaught std::terminate.
    ScopedEnv override_dir("MINIMAL_CONFIG_DIR", nullptr);
    ScopedEnv xdg("XDG_CONFIG_HOME", "/tmp/xdg-no-home");
    ScopedEnv home("HOME", nullptr);
    CHECK(ConfigDir() == "/tmp/xdg-no-home/minimal");
}

TEST_CASE("HOME unset does not stop an absolute XDG_DATA_HOME from answering",
          "[config_paths][invariant]") {
    ScopedEnv override_dir("MINIMAL_DATA_DIR", nullptr);
    ScopedEnv xdg("XDG_DATA_HOME", "/tmp/xdg-data-no-home");
    ScopedEnv home("HOME", nullptr);
    CHECK(DataDir() == "/tmp/xdg-data-no-home/minimal");
}

TEST_CASE("HOME unset still throws when nothing else answers", "[config_paths][invariant]") {
    // The lazy fallback only defers evaluating $HOME; it must still throw
    // once every other candidate (the override, an absolute XDG value) has
    // been tried and failed to answer.
    ScopedEnv override_dir("MINIMAL_CONFIG_DIR", nullptr);
    ScopedEnv xdg("XDG_CONFIG_HOME", nullptr);
    ScopedEnv home("HOME", nullptr);
    CHECK_THROWS_AS(ConfigDir(), std::runtime_error);
}
