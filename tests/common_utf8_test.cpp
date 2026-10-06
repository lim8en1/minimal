// minimal/common/utf8.hpp: the scrub that keeps proto3 string fields
// parseable, and the tail hold-back that keeps a character split between two
// reads intact.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "minimal/common/utf8.hpp"

using minimal::common::IncompleteUtf8Tail;
using minimal::common::ScrubUtf8;

namespace {
const std::string kReplacement = "\xEF\xBF\xBD";
}  // namespace

TEST_CASE("ScrubUtf8 keeps valid text unchanged", "[utf8]") {
    const std::string text = "plain, caf\xC3\xA9, \xE2\x82\xAC, \xF0\x9F\x98\x80";
    CHECK(ScrubUtf8(text) == text);
    CHECK(ScrubUtf8("").empty());
}

TEST_CASE("ScrubUtf8 replaces each ill-formed subsequence once", "[utf8]") {
    CHECK(ScrubUtf8("a\xFF" "b") == "a" + kReplacement + "b");            // never valid
    CHECK(ScrubUtf8("a\x80" "b") == "a" + kReplacement + "b");            // stray continuation
    CHECK(ScrubUtf8("\xE9t\xE9") == kReplacement + "t" + kReplacement);   // Latin-1
    CHECK(ScrubUtf8("\xC0\xAF") == kReplacement + kReplacement);          // overlong
    CHECK(ScrubUtf8("\xED\xA0\x80") == kReplacement + kReplacement + kReplacement);   // surrogate
    CHECK(ScrubUtf8("\xE2\x82" "x") == kReplacement + "x");               // truncated: one U+FFFD
    CHECK(ScrubUtf8("x\xF0\x9F\x98") == "x" + kReplacement);              // cut at the end
}

TEST_CASE("IncompleteUtf8Tail holds back only a character cut by the end", "[utf8]") {
    CHECK(IncompleteUtf8Tail("abc") == 0);
    CHECK(IncompleteUtf8Tail("ab\xE2\x82\xAC") == 0);   // complete
    CHECK(IncompleteUtf8Tail("ab\xE2") == 1);
    CHECK(IncompleteUtf8Tail("ab\xE2\x82") == 2);
    CHECK(IncompleteUtf8Tail("\xF0\x9F\x98") == 3);
    CHECK(IncompleteUtf8Tail("ab\xFF") == 0);            // not a prefix of anything: scrubbed instead
    CHECK(IncompleteUtf8Tail("ab\xE2x") == 0);           // broken, not cut

    // Two chunks of one euro sign reassemble to the character, not U+FFFD.
    std::string first = "price \xE2\x82";
    const std::size_t tail = IncompleteUtf8Tail(first);
    std::string pending = first.substr(first.size() - tail);
    first.resize(first.size() - tail);
    CHECK(ScrubUtf8(first) == "price ");
    CHECK(ScrubUtf8(pending + "\xAC" "5") == "\xE2\x82\xAC" "5");
}

TEST_CASE("Utf8FloorToBoundary and Utf8CeilToBoundary never split a character", "[utf8]") {
    using minimal::common::Utf8CeilToBoundary;
    using minimal::common::Utf8FloorToBoundary;
    // "a" + U+1F600 (4 bytes, indices 1..4) + "b"
    const std::string text = "a\xF0\x9F\x98\x80" "b";
    CHECK(Utf8FloorToBoundary(text, 0) == 0);
    CHECK(Utf8FloorToBoundary(text, 1) == 1);
    for (std::size_t inside = 2; inside <= 4; ++inside) {
        CHECK(Utf8FloorToBoundary(text, inside) == 1);
        CHECK(Utf8CeilToBoundary(text, inside) == 5);
    }
    CHECK(Utf8CeilToBoundary(text, 5) == 5);
    CHECK(Utf8FloorToBoundary(text, 99) == text.size());
    CHECK(Utf8CeilToBoundary(text, 99) == text.size());
}

TEST_CASE("the boundary walks are bounded on malformed runs of continuation bytes", "[utf8]") {
    using minimal::common::Utf8CeilToBoundary;
    using minimal::common::Utf8FloorToBoundary;
    // Longer than any well-formed sequence: the cut stands instead of the
    // walk eating an arbitrary amount of binary junk.
    const std::string junk(16, '\x80');
    CHECK(Utf8FloorToBoundary(junk, 10) == 10);
    CHECK(Utf8CeilToBoundary(junk, 5) == 5);
}

// --- the primitives: the one home of the bit layout every encoder/decoder in
// the tree goes through.

TEST_CASE("AppendUtf8 encodes every length and refuses what has no encoding", "[utf8]") {
    using minimal::common::AppendUtf8;
    std::string out;
    CHECK(AppendUtf8(out, U'A'));
    CHECK(AppendUtf8(out, U'é'));       // 2 bytes
    CHECK(AppendUtf8(out, U'€'));       // 3 bytes
    CHECK(AppendUtf8(out, U'\U0001F680'));   // 4 bytes
    CHECK(out == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x9A\x80");
    std::string refused;
    CHECK_FALSE(AppendUtf8(refused, 0xD800));     // surrogate
    CHECK_FALSE(AppendUtf8(refused, 0x110000));   // past U+10FFFF
    CHECK(refused.empty());
}

TEST_CASE("Utf8Decode and Utf8LeadLength agree with the encoder", "[utf8]") {
    using minimal::common::Utf8Decode;
    using minimal::common::Utf8LeadLength;
    const std::string text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x9A\x80";
    std::size_t i = 0;
    CHECK(Utf8LeadLength(text[i]) == 1);
    CHECK(Utf8Decode(text, i) == U'A');
    CHECK(Utf8LeadLength(text[i]) == 2);
    CHECK(Utf8Decode(text, i) == U'é');
    CHECK(Utf8LeadLength(text[i]) == 3);
    CHECK(Utf8Decode(text, i) == U'€');
    CHECK(Utf8LeadLength(text[i]) == 4);
    CHECK(Utf8Decode(text, i) == U'\U0001F680');
    CHECK(i == text.size());
    // A stray continuation byte or a truncated tail costs one byte as U+FFFD.
    CHECK(Utf8LeadLength('\x80') == 0);
    const std::string bad = "\x80\xE2\x82";
    i = 0;
    CHECK(Utf8Decode(bad, i) == U'�');
    CHECK(i == 1);
    CHECK(Utf8Decode(bad, i) == U'�');
    CHECK(i == 2);
}

TEST_CASE("CountCodePoints and IsAsciiSpace never look inside a sequence", "[utf8]") {
    using minimal::common::CountCodePoints;
    using minimal::common::IsAsciiSpace;
    CHECK(CountCodePoints("") == 0);
    CHECK(CountCodePoints("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x9A\x80") == 4);
    CHECK(IsAsciiSpace(' '));
    CHECK(IsAsciiSpace('\n'));
    CHECK_FALSE(IsAsciiSpace('\xA0'));   // Latin-1 NBSP byte: part of a sequence here
    CHECK_FALSE(IsAsciiSpace('a'));
}
