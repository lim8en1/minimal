// Utf8ColumnWidth/TruncateUtf8Columns (src/client/include/minimal/client/utf8.hpp):
// codepoint-counting width measurement and ellipsis truncation, added
// because tab_bar.cpp and status_bar.cpp both used to budget/truncate labels
// by raw UTF-8 BYTE length (std::string::size()), which over-counts every
// non-ASCII character 2-4x and can cut a multi-byte sequence in half.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/utf8.hpp"

using minimal::client::TruncateUtf8Columns;
using minimal::client::Utf8ColumnWidth;

TEST_CASE("Utf8ColumnWidth counts codepoints, not bytes", "[utf8]") {
    CHECK(Utf8ColumnWidth("") == 0);
    CHECK(Utf8ColumnWidth("hello") == 5);
    // Each of these is a 3-byte UTF-8 codepoint (CJK); byte length would be 9.
    CHECK(Utf8ColumnWidth("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == 3);
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
    // Three 3-byte CJK codepoints; asking for 2 columns must cut BETWEEN
    // codepoints, not mid-byte, regardless of where byte offsets fall.
    const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    const std::string result = TruncateUtf8Columns(text, 2);
    CHECK(Utf8ColumnWidth(result) == 2);
    CHECK(result == "\xE6\x97\xA5\xE2\x80\xA6");  // first codepoint + ellipsis
}

TEST_CASE("TruncateUtf8Columns at budget 0 returns empty", "[utf8]") {
    CHECK(TruncateUtf8Columns("hello", 0) == "");
}

TEST_CASE("TruncateUtf8Columns at budget 1 returns only the ellipsis", "[utf8]") {
    CHECK(TruncateUtf8Columns("hello", 1) == "\xE2\x80\xA6");
}
