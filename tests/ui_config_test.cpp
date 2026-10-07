#include "minimal/client/ui_config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace minimal::client {

namespace {

// Isolated config dir per test: SaveThemePreference WRITES, so every case
// starts from a clean slate and cleans up after itself.
const std::filesystem::path TestDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("minimal-ui-config-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

}  // namespace

TEST_CASE("SaveThemePreference seeds a fresh ui.json with only the theme key") {
    const auto dir = TestDir("fresh");
    SaveThemePreference(dir, "lucent-crm");
    const auto config = LoadUiConfig(dir);
    CHECK(config.theme == "lucent-crm");
    std::filesystem::remove_all(dir);
}

TEST_CASE("SaveThemePreference preserves every other key in the file") {
    const auto dir = TestDir("preserve");
    {
        std::ofstream out(dir / "ui.json");
        out << R"({
  "status_segments": {"agent": false},
  "scrollback_window_max_messages": 77,
  "tool_output_buffer_bytes": 8192,
  "theme": "dark"
})";
    }
    SaveThemePreference(dir, "lucent-grn");

    const auto config = LoadUiConfig(dir);
    CHECK(config.theme == "lucent-grn");
    REQUIRE(config.status_segments.count("agent") == 1);
    CHECK(config.status_segments.at("agent").hidden);
    CHECK(config.scrollback_window_max_messages == 77);
    CHECK(config.tool_output_buffer_bytes == 8192);
    // The atomic-rename write must not leave a tmp sibling behind.
    CHECK_FALSE(std::filesystem::exists(dir / "ui.json.tmp"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("SaveThemePreference refuses to rewrite a malformed ui.json") {
    const auto dir = TestDir("malformed");
    const std::string original = "{ this is not json";
    {
        std::ofstream out(dir / "ui.json");
        out << original;
    }
    SaveThemePreference(dir, "lucent-crm");
    // Same discipline as client-plugins.json's writer: bytes we cannot parse
    // are the user's, and silently replacing them would destroy their config.
    CHECK(ReadAll(dir / "ui.json") == original);
    std::filesystem::remove_all(dir);
}

TEST_CASE("SaveThemePreference refuses to rewrite a ui.json it cannot read") {
    // root bypasses POSIX read permission checks entirely -- chmod-000 would
    // still open fine, making this test spuriously pass regardless of
    // whether the production code distinguishes "unreadable" from "empty".
    if (::geteuid() == 0) {
        WARN("skipped: running as root, which bypasses POSIX read permissions");
        return;
    }
    const auto dir = TestDir("unreadable");
    const auto path = dir / "ui.json";
    const std::string original = R"({"theme": "dark", "status_segments": {"version": false}})";
    {
        std::ofstream out(path);
        out << original;
    }
    std::filesystem::permissions(path, std::filesystem::perms::none);
    SaveThemePreference(dir, "lucent-crm");
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    // Same discipline as the malformed-JSON case: bytes we could not even
    // read are never replaced with a fresh, near-empty document.
    CHECK(ReadAll(path) == original);
    std::filesystem::remove_all(dir);
}

TEST_CASE("status_segments that is not an object is ignored, and the rest of ui.json still reads") {
    // Regression: Entries() threw outside the lenient wrapper -- the TUI
    // exited with "internal error" and `minimal status-bar` terminated.
    const auto dir = TestDir("status-segments-array");
    {
        std::ofstream out(dir / "ui.json");
        out << R"({"theme": "light", "status_segments": []})";
    }
    UiConfig config;
    REQUIRE_NOTHROW(config = LoadUiConfig(dir));
    CHECK(config.theme == "light");
    CHECK(config.status_segments.empty());
}

TEST_CASE("status_segments: false hides, an object moves field by field, a bad entry is skipped") {
    const auto dir = TestDir("status-segments");
    {
        std::ofstream out(dir / "ui.json");
        out << R"({"status_segments": {
  "agent": false,
  "cost": {"side": "right", "order": 5},
  "tokens": {"order": 15},
  "model": {},
  "activity": true,
  "todo": "left",
  "paused": {"side": "middle"}
}})";
    }
    const auto config = LoadUiConfig(dir);
    const auto& segments = config.status_segments;
    REQUIRE(segments.count("agent") == 1);
    CHECK(segments.at("agent").hidden);
    REQUIRE(segments.count("cost") == 1);
    CHECK(segments.at("cost").side == plugins::StatusPlacement::Side::kRight);
    CHECK(segments.at("cost").order == 5);
    REQUIRE(segments.count("tokens") == 1);
    CHECK_FALSE(segments.at("tokens").side.has_value());   // kept: the segment's own
    CHECK(segments.at("tokens").order == 15);
    CHECK(segments.count("model") == 1);    // nothing set: does nothing, not an error
    CHECK(segments.count("activity") == 0); // true means nothing
    CHECK(segments.count("todo") == 0);     // neither false nor an object
    CHECK(segments.count("paused") == 0);   // not a side
    std::filesystem::remove_all(dir);
}

TEST_CASE("status_format is retired: an unknown key, the rest of the file still read") {
    const auto dir = TestDir("status-format-retired");
    {
        std::ofstream out(dir / "ui.json");
        out << R"({"status_format": "{version}", "theme": "light"})";
    }
    const auto config = LoadUiConfig(dir);
    CHECK(config.theme == "light");
    CHECK(config.status_segments.empty());
    std::filesystem::remove_all(dir);
}

}  // namespace minimal::client
