// Pure-logic tier for src/client/key_event_parser.{hpp,cpp}: the real
// modifier+key parser that replaced app.cpp's closed twelve-key if-ladder.
// No FTXUI screen/terminal needed -- ftxui::Event is a plain comparable
// value type (operator== compares only the raw byte string, never the
// Character-vs-Special construction tag), so every case here is a direct
// construction + comparison.
//
// The first block locks in exact backward compatibility with every one of
// the twelve keys the old KeyToEvents recognized, byte for byte -- a parser
// that covers more keys but silently changes one of the twelve that already
// worked would be a regression wearing a feature's clothes.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/key_event_parser.hpp"

using ftxui::Event;
using minimal::client::ParseKeySpec;

namespace {
bool Has(const std::vector<Event>& events, const Event& target) {
    for (const auto& e : events) {
        if (e == target) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("ParseKeySpec: exact backward compatibility with the old 12-key table",
         "[key_event_parser][regression]") {
    CHECK(Has(ParseKeySpec("ctrl+tab"), Event::Special("\x1b[27;5;9~")));
    CHECK(Has(ParseKeySpec("ctrl+shift+tab"), Event::Special("\x1b[27;6;9~")));
    CHECK(Has(ParseKeySpec("alt+left"), Event::Special("\x1b[1;3D")));
    CHECK(Has(ParseKeySpec("alt+left"), Event::Special("\x1b\x1b[D")));
    CHECK(Has(ParseKeySpec("alt+right"), Event::Special("\x1b[1;3C")));
    CHECK(Has(ParseKeySpec("alt+right"), Event::Special("\x1b\x1b[C")));
    CHECK(Has(ParseKeySpec("alt+a"), Event::AltA));
    CHECK(Has(ParseKeySpec("ctrl+o"), Event::CtrlO));
    CHECK(Has(ParseKeySpec("ctrl+t"), Event::CtrlT));
    CHECK(Has(ParseKeySpec("ctrl+w"), Event::CtrlW));
    CHECK(Has(ParseKeySpec("ctrl+q"), Event::CtrlQ));
    CHECK(Has(ParseKeySpec("pageup"), Event::PageUp));
    CHECK(Has(ParseKeySpec("escape"), Event::Escape));
    CHECK(Has(ParseKeySpec("ctrl+c"), Event::CtrlC));
}

TEST_CASE("ParseKeySpec: plain letters and digits", "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("q"), Event::Character("q")));
    CHECK(Has(ParseKeySpec("Q"), Event::Character("q")));   // case-insensitive spec
    CHECK(Has(ParseKeySpec("5"), Event::Character("5")));
}

TEST_CASE("ParseKeySpec: shift+letter produces the uppercase character",
         "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("shift+q"), Event::Character("Q")));
}

TEST_CASE("ParseKeySpec: ctrl+letter matches FTXUI's own per-letter constants",
         "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("ctrl+p"), Event::CtrlP));
    CHECK(Has(ParseKeySpec("ctrl+z"), Event::CtrlZ));
    CHECK(Has(ParseKeySpec("ctrl+a"), Event::CtrlA));
}

TEST_CASE("ParseKeySpec: alt+letter matches FTXUI's own per-letter constants",
         "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("alt+j"), Event::AltJ));
    CHECK(Has(ParseKeySpec("alt+z"), Event::AltZ));
}

TEST_CASE("ParseKeySpec: ctrl+alt+letter matches FTXUI's own per-letter constants",
         "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("ctrl+alt+d"), Event::CtrlAltD));
    CHECK(Has(ParseKeySpec("alt+ctrl+d"), Event::CtrlAltD));   // order doesn't matter
}

TEST_CASE("ParseKeySpec: function keys f1 through f12", "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("f1"), Event::F1));
    CHECK(Has(ParseKeySpec("f5"), Event::F5));
    CHECK(Has(ParseKeySpec("f12"), Event::F12));
    CHECK(ParseKeySpec("f13").empty());
    CHECK(ParseKeySpec("f0").empty());
}

TEST_CASE("ParseKeySpec: named keys", "[key_event_parser]") {
    CHECK(Has(ParseKeySpec("tab"), Event::Tab));
    CHECK(Has(ParseKeySpec("backspace"), Event::Backspace));
    CHECK(Has(ParseKeySpec("delete"), Event::Delete));
    CHECK(Has(ParseKeySpec("del"), Event::Delete));
    CHECK(Has(ParseKeySpec("home"), Event::Home));
    CHECK(Has(ParseKeySpec("end"), Event::End));
    CHECK(Has(ParseKeySpec("pagedown"), Event::PageDown));
    CHECK(Has(ParseKeySpec("enter"), Event::Return));
    CHECK(Has(ParseKeySpec("return"), Event::Return));
    CHECK(Has(ParseKeySpec("space"), Event::Character(" ")));
}

TEST_CASE("ParseKeySpec: arrow keys generalize alt+ to all four directions",
         "[key_event_parser]") {
    // Not just left/right (the old table's special case) -- up/down too.
    CHECK(Has(ParseKeySpec("alt+up"), Event::Special("\x1b[1;3A")));
    CHECK(Has(ParseKeySpec("alt+down"), Event::Special("\x1b[1;3B")));
    CHECK(Has(ParseKeySpec("left"), Event::ArrowLeft));
    CHECK(Has(ParseKeySpec("right"), Event::ArrowRight));
    CHECK(Has(ParseKeySpec("up"), Event::ArrowUp));
    CHECK(Has(ParseKeySpec("down"), Event::ArrowDown));
    CHECK(Has(ParseKeySpec("ctrl+left"), Event::ArrowLeftCtrl));
    CHECK(Has(ParseKeySpec("ctrl+up"), Event::ArrowUpCtrl));
}

TEST_CASE("ParseKeySpec: unrecognized specs return empty, never a guess",
         "[key_event_parser]") {
    CHECK(ParseKeySpec("").empty());
    CHECK(ParseKeySpec("banana").empty());
    CHECK(ParseKeySpec("hyper+q").empty());     // unknown modifier
    CHECK(ParseKeySpec("ctrl+digit9").empty());
    CHECK(ParseKeySpec("ctrl+9").empty());      // no single standard encoding
    CHECK(ParseKeySpec("ctrl+alt+left").empty());   // no known single encoding
}
