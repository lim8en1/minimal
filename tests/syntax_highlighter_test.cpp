#include "minimal/client/render/syntax_highlighter.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace minimal::client {

namespace {

Theme TestTheme() {
    return Theme::Parse(R"({"name": "test", "colors": {}})", ColorDepth::Truecolor);
}

// Renders Highlight()'s Elements to text, the same "ToText" pattern
// client_plugins_test.cpp uses: asserts CONTENT, not visual layout.
std::string ToText(const ftxui::Elements& lines) {
    std::string out;
    for (const auto& line : lines) {
        ftxui::Screen screen(200, 1);
        ftxui::Render(screen, line);
        std::string row = screen.ToString();
        while (!row.empty() && (row.back() == ' ' || row.back() == '\r')) row.pop_back();
        out += row;
        out += '\n';
    }
    return out;
}

// Applies the span list with the same rule the renderer documents -- Plain is
// a default, not a claim; among categorized spans the later (deeper) node wins
// -- yielding (category, text) pieces. What the tests assert on: the mapping
// from node types to categories, not the FTXUI output.
std::vector<std::pair<HighlightCategory, std::string>> Resolve(
    std::string_view code, const std::vector<HighlightSpan>& spans) {
    std::vector<std::pair<HighlightCategory, std::string>> pieces;
    uint32_t pos = 0;
    for (const HighlightSpan& span : spans) {
        if (span.category == HighlightCategory::Plain) continue;
        const uint32_t begin = std::max(span.start_byte, pos);
        if (begin < span.end_byte) {
            pieces.emplace_back(span.category,
                                std::string(code.substr(begin, span.end_byte - begin)));
        }
        pos = std::max(pos, span.end_byte);
    }
    return pieces;
}

bool HasPiece(const std::vector<std::pair<HighlightCategory, std::string>>& pieces,
              HighlightCategory category, std::string_view text) {
    for (const auto& [c, t] : pieces) {
        if (c == category && t == text) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("C++ keywords, strings, comments and types are categorized") {
    const SyntaxHighlighter highlighter(TestTheme());
    const std::string code = "int main() {\n  // returns nothing\n  return 0;\n}\n";
    const auto pieces = Resolve(code, highlighter.HighlightCode(code, "cpp"));
    CHECK(HasPiece(pieces, HighlightCategory::Type, "int"));
    CHECK(HasPiece(pieces, HighlightCategory::Keyword, "return"));
    CHECK(HasPiece(pieces, HighlightCategory::Comment, "// returns nothing"));
    CHECK(HasPiece(pieces, HighlightCategory::Number, "0"));
}

TEST_CASE("Aliases resolve to the vendored grammars") {
    const SyntaxHighlighter highlighter(TestTheme());
    const std::string code = "def f():\n    return 1\n";
    const auto pieces = Resolve(code, highlighter.HighlightCode(code, "py"));
    CHECK(HasPiece(pieces, HighlightCategory::Keyword, "def"));
    CHECK(highlighter.Supports("c++"));
    CHECK(highlighter.Supports("sh"));
    CHECK(!highlighter.Supports("cobol"));
}

TEST_CASE("An unsupported language is one Plain span over the whole input") {
    const SyntaxHighlighter highlighter(TestTheme());
    const std::string code = "some [unusual] source\n";
    const auto spans = highlighter.HighlightCode(code, "cobol");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].category == HighlightCategory::Plain);
    CHECK(spans[0].start_byte == 0);
    CHECK(spans[0].end_byte == code.size());
}

TEST_CASE("Every vendored language categorizes its comment") {
    // Guards the table drift failure mode: a grammar update renames node types
    // silently (unlisted -> Plain), and the curated set is only worth shipping
    // if the basics still land.
    const SyntaxHighlighter highlighter(TestTheme());
    const struct {
        const char* language;
        const char* code;
        const char* comment_text;
    } kCases[] = {
        {"cpp", "// hi\n", "// hi"},
        {"python", "# hi\n", "# hi"},
        {"javascript", "// hi\n", "// hi"},
        {"typescript", "// hi\n", "// hi"},
        {"bash", "# hi\n", "# hi"},
        {"rust", "// hi\n", "// hi"},
        {"go", "// hi\n", "// hi"},
    };
    for (const auto& test_case : kCases) {
        INFO(test_case.language);
        const auto pieces =
            Resolve(test_case.code, highlighter.HighlightCode(test_case.code, test_case.language));
        CHECK(HasPiece(pieces, HighlightCategory::Comment, test_case.comment_text));
    }
}

// B2: Highlight() memoizes its result on (language, code) -- a completed code
// block is byte-identical between a streaming message's 120 ms debounce
// ticks, so re-running the tree-sitter traversal AND rebuilding every
// ftxui::text Element for it was pure waste. This checks the cache SERVES
// THE RIGHT CONTENT, not merely that it's fast: two different snippets in the
// same language must not collide, and re-highlighting the FIRST one after the
// SECOND (a cache hit past the most-recent entry) must still return its own
// text, not the second's.
TEST_CASE("Highlight caches by (language, code) without cross-contaminating entries") {
    // A NAMED local, not TestTheme() inline: SyntaxHighlighter stores
    // `const Theme&`, and a reference member initialized from a temporary
    // constructor ARGUMENT is not lifetime-extended by that -- only a
    // reference bound directly to a temporary is. `HighlightCode()` (every
    // other test in this file) never touches theme_, so that dangling
    // reference stayed latent; Highlight() does (color_of/ColorFor), and
    // this is the first test in the file to call it.
    const Theme theme = TestTheme();
    const SyntaxHighlighter highlighter(theme);
    const std::string code_a = "int a() { return 1; }\n";
    const std::string code_b = "int b() { return 2; }\n";

    const std::string first_a = ToText(highlighter.Highlight(code_a, "cpp"));
    CHECK(first_a.find("return 1") != std::string::npos);

    const std::string first_b = ToText(highlighter.Highlight(code_b, "cpp"));
    CHECK(first_b.find("return 2") != std::string::npos);
    CHECK(first_b != first_a);

    // A repeat of `code_a` -- now the SECOND-most-recent cache entry -- must
    // still resolve to its own content, not `code_b`'s.
    const std::string second_a = ToText(highlighter.Highlight(code_a, "cpp"));
    CHECK(second_a == first_a);

    // Same code, different language key: must not collide on `code` alone.
    const std::string as_python = ToText(highlighter.Highlight(code_a, "python"));
    CHECK_FALSE(as_python.empty());
}

// The /theme swap path replaces the referenced Theme's CONTENTS in place and
// calls OnThemeChanged(); highlight_cache_ bakes the old colors into its
// cached Elements, so without the clear, every cached block would keep the
// previous theme's palette. First cell's foreground is a robust proxy: "int"
// is a Keyword span, colored when the theme supplies syntax_keyword.
TEST_CASE("OnThemeChanged drops color-baked cache entries after a theme swap") {
    const auto fg_at = [](const ftxui::Elements& lines) -> std::optional<ftxui::Color> {
        ftxui::Screen screen(80, 1);
        ftxui::Render(screen, lines.at(0));
        return screen.PixelAt(0, 0).foreground_color;
    };

    Theme swapped = Theme::Parse(R"({"name": "a", "colors": {"syntax_keyword": "#ff0000"}})",
                                 ColorDepth::Truecolor);
    SyntaxHighlighter highlighter(swapped);
    // "return" is a Keyword node in the vendored C++ grammar ("int" would be
    // a Type, a different role); the theme below supplies only
    // syntax_keyword, so an uncategorized cell would read as Default here --
    // which is the failure mode this assertion exists to catch.
    const std::string code = "return a;\n";

    const auto before = fg_at(highlighter.Highlight(code, "cpp"));
    REQUIRE(before.has_value());
    CHECK(*before == ftxui::Color(255, 0, 0));

    // The swap itself: same object (the reference the highlighter holds stays
    // valid), new contents. Until OnThemeChanged runs, the cache must still
    // serve the OLD color -- that is exactly what the clear fixes.
    swapped = Theme::Parse(R"({"name": "b", "colors": {"syntax_keyword": "#00ff00"}})",
                           ColorDepth::Truecolor);
    const auto stale = fg_at(highlighter.Highlight(code, "cpp"));
    REQUIRE(stale.has_value());
    CHECK(*stale == ftxui::Color(255, 0, 0));

    highlighter.OnThemeChanged();
    const auto fresh = fg_at(highlighter.Highlight(code, "cpp"));
    REQUIRE(fresh.has_value());
    CHECK(*fresh == ftxui::Color(0, 255, 0));
}

}  // namespace minimal::client
