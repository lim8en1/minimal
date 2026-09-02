#pragma once

// The ASCII-logo generator: pure std, no FTXUI and no plugin ABI, so the
// plugin's test compiles it directly (the established pattern for plugin
// logic -- see tests/CMakeLists.txt) and asserts on the art itself.
//
// The word is rendered from a built-in 5-row glyph set; every glyph cell is
// filled with a character drawn uniformly from the active style's charset --
// which is what makes every start (and, with per-session regeneration, every
// new session) look different: a random STYLE plus a random fill per cell,
// deterministic for a given seed.

#include <cstdint>
#include <string>
#include <vector>

namespace minimal::logo {

// One style. Two kinds:
//
//   SPACE styles: glyph cells draw from `charset`, blanks stay blank.
//
//   MARKED styles (`marker_pool` non-empty): each generation draws ONE marker
//   from `marker_pool` and every glyph cell is that marker; blanks fill with
//   random characters from `noise_charset` (which never contains the marker).
//   The word reads because it is the only uniform symbol in the block -- the
//   "readable noise" look.
struct LogoStyle {
    std::string name;
    std::string charset;        // space styles: the glyph fill pool
    std::string marker_pool;    // marked styles: the marker is drawn from here
    std::string noise_charset;  // marked styles: the background fill pool
};

// The built-in style table, in canonical order.
const std::vector<LogoStyle>& BuiltinStyles();

struct GenerateOptions {
    std::string text = "minimal";
    // Names of the styles a caller allows; empty = every built-in style.
    std::vector<std::string> styles;
    // Non-empty overrides every SPACE style's charset and every MARKED
    // style's marker pool with this one.
    std::string charset;
};

// Renders `options.text` in the named style, one string per row, all rows the
// same width. Deterministic for (options, style_name, seed). Throws
// std::invalid_argument naming the style when the name matches nothing
// allowed (unknown, or not in options.styles), when a glyph of the text has
// no art, or when the effective charset is empty.
std::vector<std::string> GenerateLogo(const GenerateOptions& options,
                                      const std::string& style_name, std::uint32_t seed);

// The widest row -- the width a caller degrades on (a terminal narrower than
// this plus margins gets a plain-text fallback instead of wrapped art).
std::size_t ArtWidth(const std::vector<std::string>& art);

// True when every character of `text` has a glyph (a registration-time config
// check; GenerateLogo throws on the same condition, so a plugin can warn at
// load instead of failing silently per frame).
bool CanRender(const std::string& text);

}  // namespace minimal::logo
