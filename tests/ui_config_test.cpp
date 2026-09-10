#include "minimal/client/ui_config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

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
  "status_format": "{version} | {tokens}",
  "scrollback_window_max_messages": 77,
  "tool_output_buffer_bytes": 8192,
  "theme": "dark"
})";
    }
    SaveThemePreference(dir, "lucent-grn");

    const auto config = LoadUiConfig(dir);
    CHECK(config.theme == "lucent-grn");
    CHECK(config.status_format == "{version} | {tokens}");
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

}  // namespace minimal::client
