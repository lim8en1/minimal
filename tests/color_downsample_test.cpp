#include "minimal/client/sdk/color_downsample.hpp"

#include <catch2/catch_test_macros.hpp>

namespace minimal::client {

TEST_CASE("ColorDepthFromEnv detects terminal capability") {
    CHECK(ColorDepthFromEnv("truecolor", "xterm") == ColorDepth::Truecolor);
    CHECK(ColorDepthFromEnv("24bit", nullptr) == ColorDepth::Truecolor);
    CHECK(ColorDepthFromEnv(nullptr, "xterm-256color") == ColorDepth::Palette256);
    CHECK(ColorDepthFromEnv("", "screen-256color") == ColorDepth::Palette256);
    CHECK(ColorDepthFromEnv(nullptr, "xterm") == ColorDepth::Palette16);
    CHECK(ColorDepthFromEnv(nullptr, nullptr) == ColorDepth::Palette16);
    // COLORTERM wins over TERM when both are present.
    CHECK(ColorDepthFromEnv("truecolor", "xterm") == ColorDepth::Truecolor);
    // An unrecognized COLORTERM does not block the TERM check.
    CHECK(ColorDepthFromEnv("other", "xterm-256color") == ColorDepth::Palette256);
}

TEST_CASE("Downsample is identity at truecolor") {
    CHECK(Downsample(125, 200, 55, ColorDepth::Truecolor) == ftxui::Color(125, 200, 55));
}

TEST_CASE("Downsample to 256 colors picks the nearest palette entry") {
    // Pure primaries land exactly on cube corners.
    CHECK(Downsample(255, 0, 0, ColorDepth::Palette256) ==
          ftxui::Color(ftxui::Color::Palette256(196)));
    CHECK(Downsample(255, 255, 255, ColorDepth::Palette256) ==
          ftxui::Color(ftxui::Color::Palette256(231)));
    CHECK(Downsample(0, 0, 0, ColorDepth::Palette256) ==
          ftxui::Color(ftxui::Color::Palette256(16)));
    // A mid gray is closer to the gray ramp than to the cube's diagonal.
    // Mean 128 -> ramp level 128 (8 + 10*12) -> index 232 + 12.
    CHECK(Downsample(128, 128, 128, ColorDepth::Palette256) ==
          ftxui::Color(ftxui::Color::Palette256(244)));
}

TEST_CASE("Downsample to 16 colors picks the nearest palette entry") {
    CHECK(Downsample(255, 0, 0, ColorDepth::Palette16) ==
          ftxui::Color(ftxui::Color::Red));  // (170,0,0) beats (255,85,85)
    CHECK(Downsample(255, 255, 255, ColorDepth::Palette16) ==
          ftxui::Color(ftxui::Color::White));
    CHECK(Downsample(0, 0, 0, ColorDepth::Palette16) == ftxui::Color(ftxui::Color::Black));
    CHECK(Downsample(0, 0, 255, ColorDepth::Palette16) ==
          ftxui::Color(ftxui::Color::Blue));  // (0,0,170) beats (85,85,255)
}

TEST_CASE("Downsample at ColorDepth::None emits no color at all") {
    // Not a gray: any explicit color still writes a 38;2 SGR, which is what
    // NO_COLOR forbids and what TERM=dumb prints as garbage.
    CHECK(Downsample(255, 0, 0, ColorDepth::None) == ftxui::Color::Default);
    CHECK(Downsample(128, 128, 128, ColorDepth::None) == ftxui::Color::Default);
}

    TEST_CASE("NO_COLOR and TERM=dumb disable color entirely") {
        // The de-facto NO_COLOR standard and TERM=dumb were both ignored: colors
        // were emitted regardless, so a piped/monochrome terminal got SGR noise.
        CHECK(ColorDepthFromEnv("truecolor", "xterm-256color", "1") == ColorDepth::None);
        CHECK(ColorDepthFromEnv(nullptr, "dumb") == ColorDepth::None);
        // NO_COLOR set but EMPTY is "unset" per the standard.
        CHECK(ColorDepthFromEnv("truecolor", "xterm", "") == ColorDepth::Truecolor);
        // And the old two-arg call sites keep their behavior.
        CHECK(ColorDepthFromEnv("truecolor", "xterm") == ColorDepth::Truecolor);
    }
TEST_CASE("ColorDepthFromEnv recognizes a truecolor terminal without COLORTERM") {
    CHECK(ColorDepthFromEnv(nullptr, "xterm-kitty") == ColorDepth::Truecolor);
    CHECK(ColorDepthFromEnv(nullptr, "xterm-direct") == ColorDepth::Truecolor);
    CHECK(ColorDepthFromEnv(nullptr, "xterm-256color", nullptr, "iTerm.app") ==
          ColorDepth::Truecolor);
    // Apple's Terminal renders 256 colors only.
    CHECK(ColorDepthFromEnv(nullptr, "xterm-256color", nullptr, "Apple_Terminal") ==
          ColorDepth::Palette256);
    // NO_COLOR still wins over everything.
    CHECK(ColorDepthFromEnv("truecolor", "xterm-kitty", "1", "iTerm.app") == ColorDepth::None);
}

}  // namespace minimal::client
