// Utf8ColumnWidth/TruncateUtf8Columns (src/client/include/minimal/client/utf8.hpp):
// DISPLAY-COLUMN measurement and ellipsis truncation. Originally added because
// tab_bar.cpp and status_bar.cpp budgeted/truncated labels by raw UTF-8 BYTE
// length (std::string::size()), which over-counts every non-ASCII character
// 2-4x and can cut a multi-byte sequence in half; then corrected from
// codepoint counting to column counting, because CJK/emoji are 2 terminal
// columns each and codepoint counting under-measured those labels 2x.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/utf8.hpp"

using minimal::client::TruncateUtf8Columns;
using minimal::client::Utf8ColumnWidth;

TEST_CASE("Utf8ColumnWidth counts display columns, not bytes or codepoints", "[utf8]") {
    CHECK(Utf8ColumnWidth("") == 0);
    CHECK(Utf8ColumnWidth("hello") == 5);
    // Each of these is one 3-byte CJK codepoint that occupies TWO columns:
    // byte length would be 9, codepoint count would be 3.
    CHECK(Utf8ColumnWidth("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == 6);
    // Combining marks occupy no column of their own.
    CHECK(Utf8ColumnWidth("e\xCC\x81") == 1);  // e + U+0301 COMBINING ACUTE
}

TEST_CASE("TruncateUtf8Columns is a no-op under the limit", "[utf8]") {
    CHECK(TruncateUtf8Columns("hello", 10) == "hello");
    CHECK(TruncateUtf8Columns("hello", 5) == "hello");
}

TEST_CASE("TruncateUtf8Columns truncates and appends an ellipsis", "[utf8]") {
    const std::string result = TruncateUtf8Columns("hello world", 5);
    CHECK(Utf8ColumnWidth(result) == 5);
    CHECK(result == "hell\xE2\x80\xA6");  // 4 kept chars + ellipsis = 5 columns
}

TEST_CASE("TruncateUtf8Columns never splits a codepoint", "[utf8]") {
    // Three CJK codepoints, 2 columns each. Budget 2 reserves one column for
    // the ellipsis (keep = 1), so the first codepoint (2 columns) does NOT
    // fit: nothing is kept and the result is the ellipsis alone -- a wide
    // character is never half-kept.
    const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    const std::string result = TruncateUtf8Columns(text, 2);
    CHECK(result == "\xE2\x80\xA6");

    // Budget 4 (keep = 3): the first codepoint fits, the second does not.
    const std::string one = TruncateUtf8Columns(text, 4);
    CHECK(one == "\xE6\x97\xA5\xE2\x80\xA6");  // first codepoint + ellipsis
    CHECK(Utf8ColumnWidth(one) == 3);
}

TEST_CASE("TruncateUtf8Columns at budget 0 returns empty", "[utf8]") {
    CHECK(TruncateUtf8Columns("hello", 0) == "");
}

TEST_CASE("TruncateUtf8Columns at budget 1 returns only the ellipsis", "[utf8]") {
    CHECK(TruncateUtf8Columns("hello", 1) == "\xE2\x80\xA6");
}
