#pragma once

#include <cstdint>

#include <ftxui/screen/color.hpp>

namespace minimal::client {

// What the terminal can actually display. Themes are authored in truecolor and
// degraded automatically: authored once, mapped down at load time.
enum class ColorDepth { None, Truecolor, Palette256, Palette16 };

// Reads COLORTERM/TERM: truecolor iff COLORTERM is "truecolor" or "24bit", 256
// iff TERM contains "256color", else 16. NO_COLOR (set and non-empty, the
// de-facto standard) or TERM=dumb disable color entirely -- both used to be
// ignored, so a piped/monochrome terminal still got SGR sequences. Checked
// ONCE at startup, not per render.
ColorDepth DetectColorDepth();

// The env-reading split out so the decision is testable without mutating the
// process environment. Any pointer may be null (variable unset).
ColorDepth ColorDepthFromEnv(const char* colorterm, const char* term,
                             const char* no_color = nullptr,
                             const char* term_program = nullptr);

// Truecolor -> the nearest palette entry for the given depth. Identity for
// Truecolor. Nearest means smallest squared RGB distance, with the 256-color
// gray ramp considered alongside the 6x6x6 cube.
ftxui::Color Downsample(uint8_t r, uint8_t g, uint8_t b, ColorDepth depth);

}  // namespace minimal::client
