#include "minimal/client/render/color_downsample.hpp"

#include <algorithm>
#include <string_view>

#include "minimal/common/config_paths.hpp"

namespace minimal::client {

namespace {

// Squared RGB distance. Deliberately not perceptually weighted: the palette
// lookup is a startup-time convenience, not a color science problem, and the
// extra weighting buys nothing a user can see on 240 palette entries.
int Distance2(int r1, int g1, int b1, int r2, int g2, int b2) {
    const int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}

// The 6x6x6 color cube occupies indices 16..231 with channel levels
// 0, 95, 135, 175, 215, 255.
// One definition for both consumers (CubeLevel and Downsample256's distance
// check): two copies were one refactor away from disagreeing.
constexpr int kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

int CubeLevel(int channel) {
    int best = 0;
    for (int i = 1; i < 6; ++i) {
        if (std::abs(kCubeLevels[i] - channel) < std::abs(kCubeLevels[best] - channel)) best = i;
    }
    return best;
}

ftxui::Color Downsample256(uint8_t r, uint8_t g, uint8_t b) {
    const int ri = CubeLevel(r), gi = CubeLevel(g), bi = CubeLevel(b);
    const int cube_distance =
        Distance2(r, g, b, kCubeLevels[ri], kCubeLevels[gi], kCubeLevels[bi]);
    const int cube_index = 16 + 36 * ri + 6 * gi + bi;

    // The gray ramp (232..255, shades 8 + 10*i) serves near-gray colors better
    // than the cube's 26-step diagonal. Compare candidates rather than switching
    // on "is it gray enough": a hue just off the diagonal may still be closer to
    // the cube.
    const int mean = (r + g + b) / 3;
    const int i = std::min(23, std::max(0, (mean - 8 + 5) / 10));
    const int gray_level = 8 + 10 * i;
    const int gray_distance = Distance2(r, g, b, gray_level, gray_level, gray_level);

    const int index = gray_distance < cube_distance ? 232 + i : cube_index;
    return ftxui::Color::Palette256(index);
}

ftxui::Color Downsample16(uint8_t r, uint8_t g, uint8_t b) {
    // Standard xterm palette approximations. Exact values vary by terminal, so
    // precision here is not achievable; nearest-by-distance is the honest rule.
    static constexpr int kRgb[16][3] = {
        {0, 0, 0},       {170, 0, 0},     {0, 170, 0},     {170, 85, 0},
        {0, 0, 170},     {170, 0, 170},   {0, 170, 170},   {170, 170, 170},
        {85, 85, 85},    {255, 85, 85},   {85, 255, 85},   {255, 255, 85},
        {85, 85, 255},   {255, 85, 255},  {85, 255, 255},  {255, 255, 255},
    };
    int best = 0;
    for (int i = 1; i < 16; ++i) {
        if (Distance2(r, g, b, kRgb[i][0], kRgb[i][1], kRgb[i][2]) <
            Distance2(r, g, b, kRgb[best][0], kRgb[best][1], kRgb[best][2])) {
            best = i;
        }
    }
    return ftxui::Color::Palette16(best);
}

}  // namespace

ColorDepth ColorDepthFromEnv(const char* colorterm, const char* term,
                             const char* no_color) {
    // NO_COLOR: set to any non-empty value disables color (https://no-color.org
    // de-facto standard). TERM=dumb means the terminal renders no SGR at all.
    // Both used to be ignored and colors were emitted regardless.
    if (no_color != nullptr && *no_color != '\0') return ColorDepth::None;
    if (term != nullptr && std::string_view(term) == "dumb") return ColorDepth::None;
    if (colorterm != nullptr) {
        const std::string_view value(colorterm);
        if (value == "truecolor" || value == "24bit") return ColorDepth::Truecolor;
    }
    if (term != nullptr && std::string_view(term).find("256color") != std::string_view::npos) {
        return ColorDepth::Palette256;
    }
    return ColorDepth::Palette16;
}

ColorDepth DetectColorDepth() {
    // LookupEnv, not raw getenv: set-but-empty is unset (NO_COLOR= especially).
    const auto colorterm = common::LookupEnv("COLORTERM");
    const auto term = common::LookupEnv("TERM");
    const auto no_color = common::LookupEnv("NO_COLOR");
    return ColorDepthFromEnv(colorterm ? colorterm->c_str() : nullptr,
                             term ? term->c_str() : nullptr,
                             no_color ? no_color->c_str() : nullptr);
}

ftxui::Color Downsample(uint8_t r, uint8_t g, uint8_t b, ColorDepth depth) {
    switch (depth) {
        case ColorDepth::None:
            // Grayscale fallback rather than SGR suppression: the call sites
            // render with ftxui colors, so "no color" here means the nearest
            // gray for every requested RGB -- nothing ANSI is distinguishable
            // on a dumb terminal anyway, and this keeps one code path.
            return ftxui::Color(std::min(255, (r + g + b) / 3),
                                std::min(255, (r + g + b) / 3),
                                std::min(255, (r + g + b) / 3));
        case ColorDepth::Truecolor:
            return ftxui::Color(r, g, b);
        case ColorDepth::Palette256:
            return Downsample256(r, g, b);
        case ColorDepth::Palette16:
            return Downsample16(r, g, b);
    }
    return ftxui::Color(r, g, b);
}

}  // namespace minimal::client
