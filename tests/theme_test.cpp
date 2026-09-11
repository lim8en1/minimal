#include "minimal/client/render/theme.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
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

TEST_CASE("ColorDepth::None loads every role as the terminal default") {
    const auto theme = Theme::Parse(R"({"colors": {"error": "#ff0000"}})", ColorDepth::None);
    CHECK(theme.depth() == ColorDepth::None);
    CHECK(theme.ColorFor("error") == ftxui::Color::Default);
}

namespace {

ftxui::Cell RenderOneCell(const ftxui::Decorator& style) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(1), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, ftxui::text("x") | style);
    return screen.CellAt(0, 0);
}

}  // namespace

TEST_CASE("SelectionStyle uses the theme's roles and falls back to inverted") {
    const auto themed = Theme::Parse(
        R"({"colors": {"row_focused": "#ff0000", "status_bar_bg": "#000080"}})",
        ColorDepth::Truecolor);
    const ftxui::Cell cell = RenderOneCell(SelectionStyle(themed));
    CHECK_FALSE(cell.inverted);
    CHECK(cell.foreground_color == ftxui::Color(255, 0, 0));
    CHECK(cell.background_color == ftxui::Color(0, 0, 0x80));

    // Either role missing (a partial theme, or ColorDepth::None): the cursor
    // must stay visible, so the terminal's own swap is the fallback.
    const auto partial =
        Theme::Parse(R"({"colors": {"row_focused": "#ff0000"}})", ColorDepth::Truecolor);
    CHECK(RenderOneCell(SelectionStyle(partial)).inverted);
    const auto none = Theme::Parse(
        R"({"colors": {"row_focused": "#ff0000", "status_bar_bg": "#000080"}})",
        ColorDepth::None);
    CHECK(RenderOneCell(SelectionStyle(none)).inverted);
}

TEST_CASE("Muted is a theme color, never SGR dim") {
    const auto themed = Theme::Parse(
        R"({"colors": {"muted": "#010203", "system_message": "#040506"}})", ColorDepth::Truecolor);
    ftxui::Cell cell = RenderOneCell(Muted(themed));
    CHECK_FALSE(cell.dim);
    CHECK(cell.foreground_color == ftxui::Color(1, 2, 3));

    // No `muted` role: system_message stands in.
    const auto fallback =
        Theme::Parse(R"({"colors": {"system_message": "#040506"}})", ColorDepth::Truecolor);
    CHECK(RenderOneCell(Muted(fallback)).foreground_color == ftxui::Color(4, 5, 6));

    // Neither (NO_COLOR): plain text -- and an enclosing tint is left alone.
    const auto none = Theme::Parse(R"({"colors": {"muted": "#010203"}})", ColorDepth::None);
    cell = RenderOneCell(Muted(none) | ftxui::color(ftxui::Color(7, 8, 9)));
    CHECK_FALSE(cell.dim);
    CHECK(cell.foreground_color == ftxui::Color(7, 8, 9));
}

TEST_CASE("FillBackground fills only cells left at the default background") {
    using namespace ftxui;
    auto screen = Screen::Create(Dimension::Fixed(3), Dimension::Fixed(1));
    Render(screen, hbox({text("a"), text("b") | bgcolor(Color(0, 255, 0)), text("c")}) |
                       FillBackground(Color(1, 2, 3)));
    CHECK(screen.CellAt(0, 0).background_color == Color(1, 2, 3));
    CHECK(screen.CellAt(1, 0).background_color == Color(0, 255, 0));  // explicit bg wins
    CHECK(screen.CellAt(2, 0).background_color == Color(1, 2, 3));

    // Default is a no-op: a theme without `background` stays transparent.
    CHECK(RenderOneCell(FillBackground(Color::Default)).background_color == Color::Default);
}

namespace {

// WCAG relative luminance + contrast ratio, for the bundled-theme check below.
double Linearize(uint8_t channel) {
    const double s = channel / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

double Luminance(const nlohmann::json& color) {
    const std::string hex = color.get<std::string>();
    const auto channel = [&](int offset) {
        return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
    };
    return 0.2126 * Linearize(channel(1)) + 0.7152 * Linearize(channel(3)) +
           0.0722 * Linearize(channel(5));
}

double Contrast(const nlohmann::json& color, double background_luminance) {
    const double luminance = Luminance(color);
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
        "syntax_operator", "syntax_plain",      "banner_hint",   "banner_art",
        "working",         "row_focused",       "diff_added",    "diff_removed",
        "diff_hunk",       "muted",
    };
    const struct {
        const char* name;
        // The terminal backgrounds the theme is named for. Most themes pick
        // one family (dark vs black, light vs white); the mid-tone themes are
        // named for *transparent* terminals, whose backdrop is whatever the
        // desktop shows -- they must stay legible on both extremes.
        std::vector<double> background_luminances;
    } kThemes[] = {{"dark", {0.0}},
                   {"light", {1.0}},
                   // The ported opencode themes (lucent-*): same test, same
                   // bar -- a conversion typo (an unresolved def reference, a
                   // leftover "transparent") would fail the parse outright.
                   {"lucent-crm", {0.0}},
                   {"lucent-crm-light", {1.0}},
                   {"lucent-grn", {0.0}},
                   {"lucent-grn-light", {1.0}},
                   // Mid-tone themes for transparent terminal backgrounds.
                   {"slate", {0.0, 1.0}},
                   {"moss", {0.0, 1.0}}};

    for (const auto& entry : kThemes) {
        std::ifstream in(themes_dir / (std::string(entry.name) + ".json"));
        REQUIRE(in.good());
        std::ostringstream contents;
        contents << in.rdbuf();
        const auto doc = nlohmann::json::parse(contents.str());
        // A theme that paints its own canvas must also be legible on THAT.
        std::vector<double> backgrounds = entry.background_luminances;
        if (doc.at("colors").contains("background")) {
            backgrounds.push_back(Luminance(doc.at("colors").at("background")));
        }
        for (const char* role : kTextRoles) {
            for (const double background_luminance : backgrounds) {
                INFO(entry.name << " role " << role << " vs background luminance "
                                << background_luminance);
                REQUIRE(doc.at("colors").contains(role));
                CHECK(Contrast(doc.at("colors").at(role), background_luminance) >= 3.0);
            }
        }
    }
}

// Two syntax categories sharing a color are one category on screen (the
// lucent-*-light ports had functions == types and strings == operators).
// syntax_plain is exempt: operators commonly share the plain color.
TEST_CASE("Bundled themes give every syntax category its own color") {
    const std::filesystem::path themes_dir = std::filesystem::path(MINIMAL_BUNDLED_DIR) / "themes";
    const char* kSyntaxRoles[] = {"syntax_keyword",  "syntax_string", "syntax_comment",
                                  "syntax_number",   "syntax_function", "syntax_type",
                                  "syntax_operator"};
    for (const auto& file : std::filesystem::directory_iterator(themes_dir)) {
        if (file.path().extension() != ".json") continue;
        std::ifstream in(file.path());
        REQUIRE(in.good());
        std::ostringstream contents;
        contents << in.rdbuf();
        const auto colors = nlohmann::json::parse(contents.str()).at("colors");
        for (std::size_t a = 0; a < std::size(kSyntaxRoles); ++a) {
            for (std::size_t b = a + 1; b < std::size(kSyntaxRoles); ++b) {
                INFO(file.path().filename().string() << ": " << kSyntaxRoles[a] << " vs "
                                                     << kSyntaxRoles[b]);
                CHECK(colors.at(kSyntaxRoles[a]) != colors.at(kSyntaxRoles[b]));
            }
        }
    }
}

}  // namespace minimal::client
