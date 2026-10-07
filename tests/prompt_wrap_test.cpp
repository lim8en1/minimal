// Pure-logic tier for src/client/view/prompt_wrap.{hpp,cpp}: the soft-wrap
// geometry the prompt editor renders from. No FTXUI needed -- the helper is
// std-only by design (the same tier split key_event_parser uses).
//
// The invariants under test: rows tile the content byte-for-byte (every byte
// in exactly one row -- that is what makes cursor mapping plain containment),
// words break at spaces when they can and hard-break when they cannot, and a
// cursor exactly on a break boundary stays on the EARLIER row, at its end.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "prompt_wrap.hpp"

using minimal::client::prompt_editor::PromptWrapResult;
using minimal::client::prompt_editor::WrapPromptContent;

namespace {
// The rows' byte ranges must cover the content with no byte in two rows and
// no byte outside them, EXCEPT the '\n' separator bytes between logical lines
// (which no visual row shows). The editor's cursor mapping and click mapping
// both lean on this.
void CheckTiling(const std::string& content, const PromptWrapResult& result) {
    std::size_t expected = 0;
    for (const auto& row : result.lines) {
        for (std::size_t k = expected; k < row.begin; ++k) {
            CHECK(content[k] == '\n');
        }
        CHECK(row.end <= content.size());
        CHECK(row.text == content.substr(row.begin, row.end - row.begin));
        expected = row.end;
    }
    for (std::size_t k = expected; k < content.size(); ++k) {
        CHECK(content[k] == '\n');
    }
}
}  // namespace

TEST_CASE("WrapPromptContent: a short line is one row", "[prompt_wrap]") {
    const auto r = WrapPromptContent("hello", 3, 80);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].text == "hello");
    CHECK(r.cursor_row == 0);
    CHECK(r.cursor_col == 3);
    CheckTiling("hello", r);
}

TEST_CASE("WrapPromptContent: empty content is one empty row", "[prompt_wrap]") {
    const auto r = WrapPromptContent("", 0, 80);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].text.empty());
    CHECK(r.cursor_row == 0);
    CHECK(r.cursor_col == 0);
    CheckTiling("", r);
}

TEST_CASE("WrapPromptContent: word wrap breaks before the overflowing word",
          "[prompt_wrap]") {
    const std::string content = "hello world foo";
    const auto r = WrapPromptContent(content, 0, 10);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "hello ");
    CHECK(r.lines[1].text == "world foo");
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: a word wider than a row hard-breaks", "[prompt_wrap]") {
    const std::string content = "aaaaaaaaaa";   // 10, width 4
    const auto r = WrapPromptContent(content, 0, 4);
    REQUIRE(r.lines.size() == 3);
    CHECK(r.lines[0].text == "aaaa");
    CHECK(r.lines[1].text == "aaaa");
    CHECK(r.lines[2].text == "aa");
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: cursor inside a hard-broken word maps onto its row",
          "[prompt_wrap]") {
    const auto r = WrapPromptContent("abcdef", 4, 3);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "abc");
    CHECK(r.lines[1].text == "def");
    CHECK(r.cursor_row == 1);
    CHECK(r.cursor_col == 1);
}

TEST_CASE("WrapPromptContent: cursor on a break boundary stays on the earlier row",
          "[prompt_wrap]") {
    // "aaaa bbb" at width 4: row 0 = "aaaa", row 1 = " bbb". The cursor byte
    // 4 is both row 0's end and row 1's start; it renders at row 0's end --
    // the same placement FTXUI's Input uses for a cursor at a line's end.
    const std::string content = "aaaa bbb";
    const auto r = WrapPromptContent(content, 4, 4);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.cursor_row == 0);
    CHECK(r.cursor_col == 4);
    // One byte further is on the later row, after its leading space.
    const auto r2 = WrapPromptContent(content, 5, 4);
    CHECK(r2.cursor_row == 1);
    CHECK(r2.cursor_col == 1);
}

TEST_CASE("WrapPromptContent: logical lines split at \\n, trailing newline keeps an empty row",
          "[prompt_wrap]") {
    const std::string content = "ab\ncd\n";
    const auto r = WrapPromptContent(content, content.size(), 80);
    REQUIRE(r.lines.size() == 3);
    CHECK(r.lines[0].text == "ab");
    CHECK(r.lines[1].text == "cd");
    CHECK(r.lines[2].text.empty());
    CHECK(r.cursor_row == 2);
    CHECK(r.cursor_col == 0);
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: wide code points count two columns", "[prompt_wrap]") {
    const std::string content = "字字字字";   // 4 CJK glyphs, 2 columns each
    const auto r = WrapPromptContent(content, 0, 5);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "字字");   // 4 columns; a third would be 6 > 5
    CHECK(r.lines[1].text == "字字");
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: a wide glyph is never split across rows", "[prompt_wrap]") {
    // "aaa字" is 5 columns; at width 4 the glyph does not fit after 3 used --
    // it moves whole to the next row.
    const std::string content = "aaa字";
    const auto r = WrapPromptContent(content, 0, 4);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "aaa");
    CHECK(r.lines[1].text == "字");
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: combining marks occupy no column", "[prompt_wrap]") {
    // "e" + U+0301 COMBINING ACUTE: one display column, never a break point.
    const std::string content = "e\xCC\x81!";
    const auto r = WrapPromptContent(content, 0, 2);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].text == content);
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: the break's trailing spaces stay on the earlier row",
          "[prompt_wrap]") {
    // The tiling invariant's consequence: invisible trailing spaces keep
    // every byte in a row, so the cursor can sit between "ab" and "cd".
    const std::string content = "ab    cd";   // width 4
    const auto r = WrapPromptContent(content, 6, 4);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "ab  ");
    CHECK(r.lines[1].text == "  cd");
    CHECK(r.cursor_row == 1);
    CHECK(r.cursor_col == 2);
    CheckTiling(content, r);
}

TEST_CASE("WrapPromptContent: width 0 degrades to one column", "[prompt_wrap]") {
    const auto r = WrapPromptContent("ab", 0, 0);
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].text == "a");
    CHECK(r.lines[1].text == "b");
    CheckTiling("ab", r);
}

TEST_CASE("WrapPromptContent: a cursor past the content end clamps to the last row",
          "[prompt_wrap]") {
    const auto r = WrapPromptContent("abc", 999, 80);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.cursor_row == 0);
    CHECK(r.cursor_col == 3);
}
