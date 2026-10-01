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
