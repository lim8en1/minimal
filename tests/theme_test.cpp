#include "minimal/client/render/theme.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

namespace minimal::client {

TEST_CASE("Theme parses roles and applies the color depth at load") {
    const auto theme = Theme::Parse(R"({
        "name": "test",
        "colors": {"user_message": "#ff0000", "error": "#0f0"}
    })",
                                    ColorDepth::Palette256);
    CHECK(theme.name() == "test");
    CHECK(theme.size() == 2);
    // Downsampled at load, not at query: truecolor #ff0000 became palette 196.
    CHECK(theme.ColorFor("user_message") == ftxui::Color(ftxui::Color::Palette256(196)));
    CHECK(theme.ColorFor("error") == ftxui::Color(ftxui::Color::Palette256(46)));
    // A role the theme does not set falls back to the terminal default.
    CHECK(theme.ColorFor("attention") == ftxui::Color::Default);
}

TEST_CASE("Theme keeps roles nothing in this build reads") {
    // Forward compatibility: a theme shared with a newer build must stay
    // loadable rather than erroring on the unknown role.
    const auto theme = Theme::Parse(R"({
        "name": "t", "colors": {"future_role": "#123456"}
    })",
                                    ColorDepth::Truecolor);
    CHECK(theme.ColorFor("future_role") == ftxui::Color(0x12, 0x34, 0x56));
}

TEST_CASE("Theme rejects malformed colors, naming the role") {
    CHECK_THROWS_WITH(Theme::Parse(R"({"colors": {"error": "red"}})", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("error"));
    CHECK_THROWS_WITH(Theme::Parse(R"({"colors": {"error": "#gg0000"}})", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("error"));
    CHECK_THROWS_WITH(Theme::Parse(R"({"colors": {"error": 12}})", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("error"));
    CHECK_THROWS_WITH(Theme::Parse("not json", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("malformed JSON"));
    CHECK_THROWS_WITH(Theme::Parse(R"({"name": "x"})", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("colors"));
}

TEST_CASE("Theme::Load reads <dir>/<name>.json and reports the path on failure") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal-theme-test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "unit.json");
        out << R"({"name": "unit", "colors": {"error": "#ff0000"}})";
    }
    const auto theme = Theme::Load(dir, "unit", ColorDepth::Truecolor);
    CHECK(theme.ColorFor("error") == ftxui::Color(255, 0, 0));

    CHECK_THROWS_WITH(Theme::Load(dir, "missing", ColorDepth::Truecolor),
                      Catch::Matchers::ContainsSubstring("missing.json"));
    std::filesystem::remove_all(dir);
}

namespace {

// WCAG relative luminance + contrast ratio, for the bundled-theme check below.
double Linearize(uint8_t channel) {
    const double s = channel / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

double Contrast(const nlohmann::json& color, double background_luminance) {
    const std::string hex = color.get<std::string>();
    const auto channel = [&](int offset) {
        return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
    };
    const double luminance = 0.2126 * Linearize(channel(1)) + 0.7152 * Linearize(channel(3)) +
                             0.0722 * Linearize(channel(5));
    const double hi = std::max(luminance, background_luminance);
    const double lo = std::min(luminance, background_luminance);
    return (hi + 0.05) / (lo + 0.05);
}

}  // namespace

// Every text-carrying role must stay legible on the terminal background family
// the theme is named for: a color picked against one background silently breaks
// on the other. Background/border roles are excluded -- they are surfaces, not
// text. Threshold 3.0: the WCAG large-text floor, which is the right bar for
// chat text on a terminal.
TEST_CASE("Bundled themes keep text roles legible on their background family") {
    const std::filesystem::path themes_dir = std::filesystem::path(MINIMAL_BUNDLED_DIR) / "themes";
    const char* kTextRoles[] = {
        "user_message",    "assistant_message", "thinking",      "system_message",
        "error",           "attention",         "syntax_keyword", "syntax_string",
        "syntax_comment",  "syntax_number",     "syntax_function", "syntax_type",
        "syntax_operator",
    };
    const struct {
        const char* name;
        double background_luminance;  // dark theme vs #000, light vs #fff
    } kThemes[] = {{"dark", 0.0}, {"light", 1.0}};

    for (const auto& entry : kThemes) {
        std::ifstream in(themes_dir / (std::string(entry.name) + ".json"));
        REQUIRE(in.good());
        std::ostringstream contents;
        contents << in.rdbuf();
        const auto doc = nlohmann::json::parse(contents.str());
        for (const char* role : kTextRoles) {
            INFO(entry.name << " role " << role);
            REQUIRE(doc.at("colors").contains(role));
            CHECK(Contrast(doc.at("colors").at(role), entry.background_luminance) >= 3.0);
        }
    }
}

}  // namespace minimal::client
