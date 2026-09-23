#include "logo_generator.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

namespace minimal::logo {
namespace {

// 5-row glyphs; '#' marks a fillable cell, '.' a blank. One string per row.
// 'i' keeps its dot and gap so it stays distinct from 'l' even when both are
// one column wide -- with random fill the shape (density pattern) is what
// reads, not any single character.
const std::map<char, std::vector<std::string>>& Glyphs() {
    static const std::map<char, std::vector<std::string>> glyphs = {
        {'m',
         {
             "#...#",
             "##.##",
             "#.#.#",
             "#...#",
             "#...#",
         }},
        {'i',
         {
             "#",
             ".",
             "#",
             "#",
             "#",
         }},
        {'n',
         {
             "#...#",
             "##..#",
             "#.#.#",
             "#..##",
             "#...#",
         }},
        {'a',
         {
             ".###.",
             "....#",
             ".####",
             "#...#",
             ".####",
         }},
        {'l',
         {
             "#",
             "#",
             "#",
             "#",
             "#",
         }},
    };
    return glyphs;
}

constexpr int kGlyphRows = 5;
constexpr int kLetterGap = 1;  // blank columns between letters

const std::vector<LogoStyle>& Styles() {
    static const std::vector<LogoStyle> styles = {
        // Space styles: glyphs filled from the charset, blanks stay blank.
        {"blocks", "#@%&$8WB0QD", "", ""},
        {"runes", "@#%&*+=~^", "", ""},
        {"digits", "0123456789", "", ""},
        {"binary", "01", "", ""},
        {"hex", "0123456789ABCDEF", "", ""},
        {"letters", "abcdefghijklmnopqrstuvwxyz", "", ""},
        {"symbols", "!?;:^~+-*=<>", "", ""},
        {"shades", "#*+:.", "", ""},
        {"strokes", "|/-\\_", "", ""},
        {"brackets", "[]{}()<>", "", ""},
        {"solid", "#", "", ""},
        // Marked styles: one marker draws the whole word; noise fills the
        // rest. Readability comes from the marker being the only uniform
        // symbol in the block -- the shared marker pool is heavy glyphs
        // only, so the word stays visually dominant against either noise.
        {"noise", "", "#*&^%$@!", "abcdefghijklmnopqrstuvwxyz0123456789"},
        {"static", "", "#*&^%$@!", " .,:;-'"},
    };
    return styles;
}

}  // namespace

const std::vector<LogoStyle>& BuiltinStyles() { return Styles(); }

std::vector<std::string> GenerateLogo(const GenerateOptions& options,
                                      const std::string& style_name, std::uint32_t seed) {
    const auto& styles = Styles();
    const auto style_it = std::find_if(styles.begin(), styles.end(),
                                       [&](const LogoStyle& style) {
                                           return style.name == style_name;
                                       });
    if (style_it == styles.end()) {
        throw std::invalid_argument("unknown logo style: " + style_name);
    }
    if (!options.styles.empty() &&
        std::find(options.styles.begin(), options.styles.end(), style_name) ==
            options.styles.end()) {
        throw std::invalid_argument("logo style not in the allowed set: " + style_name);
    }

    // The effective pools. A space style fills glyphs from its charset; a
    // marked style draws ONE marker and fills glyphs with it, noise from the
    // background pool. An options.charset override replaces the space style's
    // charset / the marked style's marker pool (the noise pool stays the
    // style's own -- an override that also chose the noise could not keep the
    // marker exclusion guarantee).
    const bool marked = !style_it->marker_pool.empty();
    const std::string& glyph_pool = !options.charset.empty()
                                        ? options.charset
                                        : (marked ? style_it->marker_pool : style_it->charset);
    if (glyph_pool.empty()) {
        throw std::invalid_argument("logo style has an empty charset: " + style_name);
    }
    std::string noise_pool;
    if (marked) {
        noise_pool = style_it->noise_charset;
        // Defensive, not decorative: a marker leaking into the noise pool
        // would erode exactly the uniformity the style's readability rests
        // on. (The built-in tables never overlap; a future table edit cannot
        // silently break the guarantee.)
        std::string cleaned;
        for (const char ch : noise_pool) {
            if (glyph_pool.find(ch) == std::string::npos) cleaned.push_back(ch);
        }
        if (cleaned.empty()) {
            throw std::invalid_argument("logo style has an empty noise pool: " + style_name);
        }
        noise_pool = std::move(cleaned);
    }

    // Assemble the glyph grid first (shape), then fill (randomness) -- so a
    // fill character can never alter the layout. Glyph blanks materialize as
    // real spaces: the '.' in a glyph table is a marker, never output, and a
    // '.' that leaked into most styles' art would read as a fill character.
    std::vector<std::string> grid;
    for (int row = 0; row < kGlyphRows; ++row) {
        std::string line;
        for (const char ch : options.text) {
            if (const auto glyph = Glyphs().find(ch); glyph != Glyphs().end()) {
                if (row < static_cast<int>(glyph->second.size())) {
                    line += glyph->second[static_cast<std::size_t>(row)];
                }
            } else {
                throw std::invalid_argument(std::string("no logo glyph for '") + ch + "'");
            }
            line.append(static_cast<std::size_t>(kLetterGap), ' ');
        }
        grid.push_back(std::move(line));
    }

    // Trim trailing blank COLUMNS across the whole grid (not per line), then
    // pad every row back to that width: the contract is a uniform block,
    // which is what centering and the width check want.
    std::size_t width = 0;
    for (const auto& line : grid) {
        if (const auto last = line.find_last_not_of(" ."); last != std::string::npos) {
            width = std::max(width, last + 1);
        }
    }
    if (width == 0) {
        throw std::invalid_argument("logo text renders no filled cells: " + options.text);
    }
    for (auto& line : grid) {
        line.resize(width, ' ');
        std::replace(line.begin(), line.end(), '.', ' ');
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, glyph_pool.size() - 1);
    // MARKED: one draw decides the whole word's symbol; everything else is
    // noise. SPACE: every glyph cell draws independently. noise_pick is only
    // consumed under `marked`.
    const char marker = marked ? glyph_pool[pick(rng)] : '\0';
    // Constructed only under `marked` (like `marker` above): a space style
    // reaches here with noise_pool genuinely empty (never populated above),
    // and `0, size() - 1` on an empty pool underflows to `0, SIZE_MAX` --
    // harmless only because the sole consumer below is itself gated on
    // `marked`, one refactor away from reading a garbage index if that ever
    // changed. `marked` guarantees a non-empty noise_pool (the empty-cleaned
    // check above throws otherwise), so this constructs a genuine range.
    // Not const: operator() mutates the distribution's internal state.
    std::optional<std::uniform_int_distribution<std::size_t>> noise_pick =
        marked ? std::make_optional<std::uniform_int_distribution<std::size_t>>(
                     0, noise_pool.size() - 1)
               : std::nullopt;
    for (auto& line : grid) {
        for (char& cell : line) {
            if (cell == '#') {
                cell = marked ? marker : glyph_pool[pick(rng)];
            } else if (marked) {
                cell = noise_pool[(*noise_pick)(rng)];
            }
        }
    }
    return grid;
}

std::size_t ArtWidth(const std::vector<std::string>& art) {
    std::size_t width = 0;
    for (const auto& line : art) {
        width = std::max(width, line.size());
    }
    return width;
}

bool CanRender(const std::string& text) {
    if (text.empty()) return false;
    const auto& glyphs = Glyphs();
    return std::all_of(text.begin(), text.end(),
                       [&](char ch) { return glyphs.find(ch) != glyphs.end(); });
}

}  // namespace minimal::logo
