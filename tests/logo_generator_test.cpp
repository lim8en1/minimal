// The logo generator's pure logic: glyph layout, charset discipline, seed
// determinism, and the failure modes the plugin relies on (unknown style,
// unrenderable text, empty charset). FTXUI is deliberately absent -- this
// asserts on the art itself, the way chat_model tests assert on rows.
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "logo_generator.hpp"

using namespace minimal::logo;

namespace {

// Every non-blank cell of a SPACE style's art must draw from its charset: the
// layout is assembled before the fill (the generator's own contract), but a
// test that only checked line widths would miss a fill leaking outside the
// glyphs. Marked styles additionally fill the background from their noise
// pool, so the caller passes the union.
bool CellsDrawnFrom(const std::vector<std::string>& art, const std::string& allowed) {
    for (const auto& line : art) {
        for (const char cell : line) {
            if (allowed.find(cell) == std::string::npos) return false;
        }
    }
    // And the art is non-empty.
    return !art.empty();
}

std::string StylePool(const minimal::logo::LogoStyle& style) {
    return style.charset + style.marker_pool + style.noise_charset;
}

std::map<char, int> CharCounts(const std::vector<std::string>& art) {
    std::map<char, int> counts;
    for (const auto& line : art) {
        for (const char cell : line) ++counts[cell];
    }
    return counts;
}

}  // namespace

TEST_CASE("logo generator: layout is a uniform 5-row block", "[logo]") {
    const auto art = GenerateLogo(GenerateOptions{}, "blocks", 42);
    REQUIRE(art.size() == 5);
    const auto width = ArtWidth(art);
    REQUIRE(width > 0);
    for (const auto& line : art) {
        // Uniform width is what lets the plugin box/center the art without
        // per-line adjustment; a ragged row would render skewed.
        REQUIRE(line.size() == width);
    }
}

TEST_CASE("logo generator: fill characters come from the style's pools", "[logo]") {
    for (const auto& style : BuiltinStyles()) {
        const auto art = GenerateLogo(GenerateOptions{}, style.name, 7);
        INFO("style: " << style.name);
        // Space styles: every cell is charset (glyph) or blank. Marked
        // styles: the background is noise, so the whole rectangle draws from
        // the union -- and never touches anything outside the style's pools.
        CHECK(CellsDrawnFrom(art, StylePool(style) + " "));
    }
}

TEST_CASE("logo generator: a space style (empty marker_pool/noise_charset) generates without "
         "touching noise_pick",
         "[logo]") {
    // "blocks" is a SPACE style: marker_pool and noise_charset are both "".
    // The noise_pick distribution used to be constructed unconditionally
    // from `0, noise_pool.size() - 1`, underflowing to `0, SIZE_MAX` for
    // exactly this style -- harmless only because the sole consumer is
    // gated on `marked` (always false here), so it was never actually
    // sampled. This pins that a space style still generates cleanly across
    // several seeds now that the construction itself is gated too.
    for (unsigned seed = 0; seed < 20; ++seed) {
        const auto art = GenerateLogo(GenerateOptions{}, "blocks", seed);
        CHECK_FALSE(art.empty());
    }
}

TEST_CASE("logo generator: marked styles pick ONE marker and stay readable", "[logo]") {
    for (const auto& style : BuiltinStyles()) {
        if (style.marker_pool.empty()) continue;
        const auto art = GenerateLogo(GenerateOptions{}, style.name, 7);
        INFO("style: " << style.name);

        // The most frequent character must be a marker pool member by a wide
        // margin: it accounts for every glyph cell, the noise accounts for
        // the rest. That dominance IS the readability property.
        const auto counts = CharCounts(art);
        auto dominant = std::max_element(
            counts.begin(), counts.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        CHECK(style.marker_pool.find(dominant->first) != std::string::npos);
        // "minimal" has ~50 glyph cells; noise in the same char would need
        // to nearly tie, which a 30+ gap rules out deterministically enough
        // for a fixed seed.
        CHECK(dominant->second >= 30);
        for (const auto& [ch, count] : counts) {
            if (ch != dominant->first) {
                CHECK(count < dominant->second - 20);
            }
        }
    }
}

TEST_CASE("logo generator: same seed, same art; different seeds differ", "[logo]") {
    const auto a = GenerateLogo(GenerateOptions{}, "binary", 1234);
    const auto b = GenerateLogo(GenerateOptions{}, "binary", 1234);
    REQUIRE(a == b);  // determinism: the cache contract depends on it

    // "minimal" has enough glyph cells that two fills over {0,1} colliding is
    // astronomically unlikely -- a failed fixity would show as equal art.
    const auto c = GenerateLogo(GenerateOptions{}, "binary", 1235);
    CHECK(a != c);
}

TEST_CASE("logo generator: charset override applies to space styles and markers", "[logo]") {
    GenerateOptions options;
    options.charset = "xy";
    CHECK(CellsDrawnFrom(GenerateLogo(options, "hex", 99), "xy "));

    // A marked style's override replaces the MARKER POOL -- the noise pool
    // stays the style's own, so the marker-exclusion guarantee cannot be
    // broken from config. The dominant symbol must then come from the
    // override, drawn against the style's own noise.
    const auto marked = GenerateLogo(options, "noise", 99);
    const auto counts = CharCounts(marked);
    auto dominant = std::max_element(counts.begin(), counts.end(),
                                     [](const auto& a, const auto& b) {
                                         return a.second < b.second;
                                     });
    CHECK(std::string("xy").find(dominant->first) != std::string::npos);
}

TEST_CASE("logo generator: allowed-set restriction is enforced both ways", "[logo]") {
    GenerateOptions options;
    options.styles = {"digits"};
    CHECK_NOTHROW(GenerateLogo(options, "digits", 1));
    CHECK_THROWS_AS(GenerateLogo(options, "hex", 1), std::invalid_argument);
}

TEST_CASE("logo generator: unknown style, unrenderable text, empty charset throw", "[logo]") {
    CHECK_THROWS_AS(GenerateLogo(GenerateOptions{}, "no-such-style", 1), std::invalid_argument);

    GenerateOptions options;
    options.text = "minimalQ";  // no glyph for 'Q'
    CHECK_THROWS_AS(GenerateLogo(options, "blocks", 1), std::invalid_argument);
    CHECK_FALSE(CanRender(options.text));
    CHECK(CanRender("minimal"));

    GenerateOptions blank;
    blank.charset = "";
    // Overriding with an empty string means "no override" for the PLUGIN's
    // parse, but the generator treats an explicit empty charset as its own
    // error only when the STYLE also has none -- built-ins never do, so the
    // empty-override path must fall back to the style charset.
    CHECK_NOTHROW(GenerateLogo(blank, "blocks", 1));
}

TEST_CASE("logo generator: the built-in style table is sound", "[logo]") {
    const auto& styles = BuiltinStyles();
    CHECK(styles.size() >= 8);  // "multiple styles" is the feature
    std::set<std::string> names;
    for (const auto& style : styles) {
        CHECK(names.insert(style.name).second);  // no duplicate style names
        // Every pool is non-empty (a space style has a charset; a marked
        // style has both pools) and every pool character is printable -- the
        // contract the TUI render relies on.
        if (style.marker_pool.empty()) {
            CHECK(!style.charset.empty());
            CHECK(style.noise_charset.empty());
        } else {
            CHECK(!style.noise_charset.empty());
        }
        for (const char cell : StylePool(style)) {
            CHECK(std::isprint(static_cast<unsigned char>(cell)));
        }
        // The noise pool never contains a marker: the marker's uniformity is
        // what the readability rests on.
        for (const char marker : style.marker_pool) {
            CHECK(style.noise_charset.find(marker) == std::string::npos);
        }
    }
}
