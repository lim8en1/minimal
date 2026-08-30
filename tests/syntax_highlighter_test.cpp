#include "minimal/client/render/syntax_highlighter.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace minimal::client {

namespace {

Theme TestTheme() {
    return Theme::Parse(R"({"name": "test", "colors": {}})", ColorDepth::Truecolor);
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
    CHECK(SyntaxHighlighter::Supports("c++"));
    CHECK(SyntaxHighlighter::Supports("sh"));
    CHECK(!SyntaxHighlighter::Supports("cobol"));
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

}  // namespace minimal::client
