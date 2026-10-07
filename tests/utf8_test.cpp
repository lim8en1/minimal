// Utf8ColumnWidth/TruncateUtf8Columns (src/client/include/minimal/client/sdk/utf8.hpp):
// DISPLAY-COLUMN measurement and ellipsis truncation. Originally added because
// tab_bar.cpp and status_bar.cpp budgeted/truncated labels by raw UTF-8 BYTE
// length (std::string::size()), which over-counts every non-ASCII character
// 2-4x and can cut a multi-byte sequence in half; then corrected from
// codepoint counting to column counting, because CJK/emoji are 2 terminal
// columns each and codepoint counting under-measured those labels 2x.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/sdk/utf8.hpp"

using minimal::client::SanitizeUtf8;
using minimal::client::StripControlChars;
using minimal::client::TruncateUtf8Columns;
using minimal::client::Utf8ColumnWidth;
using minimal::client::Utf8PopGrapheme;

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

// The bug: the loop checked whether the codepoint NEWLY EXPOSED after a pop
// is glue, when a variation selector or combining mark is glue for the
// codepoint BEFORE it -- so it stopped one codepoint too early every time,
// leaving the base character behind.
TEST_CASE("Utf8PopGrapheme removes a variation-selector-glued emoji as one unit",
          "[utf8]") {
    // 'x' + U+2764 HEAVY BLACK HEART + U+FE0F VARIATION SELECTOR-16.
    const std::string heart_vs16 = "x\xE2\x9D\xA4\xEF\xB8\x8F";
    CHECK(Utf8PopGrapheme(heart_vs16) == "x");
}

TEST_CASE("Utf8PopGrapheme removes a combining-mark-glued character as one unit",
          "[utf8]") {
    // 'x' + 'e' + U+0301 COMBINING ACUTE ACCENT (decomposed "xé").
    const std::string e_acute = "xe\xCC\x81";
    CHECK(Utf8PopGrapheme(e_acute) == "x");
}

TEST_CASE("Utf8PopGrapheme removes a ZWJ-joined emoji sequence as one unit", "[utf8]") {
    // man U+1F468, ZWJ U+200D, woman U+1F469, ZWJ U+200D, girl U+1F467 -- a
    // 3-person family emoji, one grapheme cluster.
    const std::string family =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    CHECK(Utf8PopGrapheme(family) == "");
}

// An untrusted display string (a server-titler-generated session name, e.g.)
// must never carry a raw ESC or newline into a terminal write: either breaks
// a table's layout or, for ESC specifically, is a terminal escape-sequence
// injection.
TEST_CASE("StripControlChars drops C0/C1 controls and DEL, keeps everything else",
          "[utf8]") {
    CHECK(StripControlChars("hello") == "hello");
    CHECK(StripControlChars("hello\x1bworld") == "helloworld");   // ESC
    CHECK(StripControlChars("line1\nline2") == "line1line2");     // newline
    CHECK(StripControlChars("a\x7f" "b") == "ab");                 // DEL
    CHECK(StripControlChars("a\xC2\x80" "b") == "ab");             // C1 control U+0080
    // Wide/multi-byte text untouched.
    CHECK(StripControlChars("\xE6\x97\xA5\x1b\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC");
}

// A raw Latin-1 paste (or any other byte-broken prompt) must degrade to the
// replacement character client-side instead of aborting the turn with a
// confusing server-side INTERNAL error over invalid UTF-8.
TEST_CASE("SanitizeUtf8 repairs malformed sequences, leaves valid input untouched",
          "[utf8]") {
    CHECK(SanitizeUtf8("hello") == "hello");
    // 0xE9 alone ('é' in Latin-1) is not a valid UTF-8 lead byte for a
    // 1-byte sequence and has no continuation byte following -- malformed.
    CHECK(SanitizeUtf8("caf\xE9") == "caf\xEF\xBF\xBD");
    // Already-valid multi-byte text passes through byte-identical.
    const std::string valid = "\xE6\x97\xA5\xE6\x9C\xAC";
    CHECK(SanitizeUtf8(valid) == valid);
}

TEST_CASE("Utf8CharColumnWidth follows the renderer's table, emoji planes included",
          "[utf8][regression]") {
    using minimal::client::Utf8CharColumnWidth;
    // The old hand-kept ranges missed the transport block: a rocket measured
    // 1 column while FTXUI drew 2, so the prompt caret drifted after one.
    CHECK(Utf8CharColumnWidth(U'\U0001F680') == 2);
    CHECK(Utf8CharColumnWidth(U'中') == 2);   // CJK, unchanged
    CHECK(Utf8CharColumnWidth(U'a') == 1);
    CHECK(Utf8CharColumnWidth(U'́') == 0);   // combining acute
}
