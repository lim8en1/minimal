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
using minimal::client::NormalizeKeySpec;
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

TEST_CASE("ParseKeySpec: shift+tab (backtab) matches both terminal encodings",
          "[key_event_parser]") {
    // permission.cycle's binding: the legacy CSI Z sequence most terminals
    // send, plus the explicit CSI-u modifier form -- Match() tries every
    // candidate, so either arrival is recognized.
    CHECK(Has(ParseKeySpec("shift+tab"), Event::TabReverse));
    CHECK(Has(ParseKeySpec("shift+tab"), Event::Special("\x1b[27;2;9~")));
}

// The bug: the tab special cases matched the raw spec STRING before modifier
// parsing ran, so a reordered modifier ("shift+ctrl+tab") or the "control"
// alias every other modifier already accepts ("control+tab") fell through
// to no mapping at all.
TEST_CASE("ParseKeySpec: tab's modifier combos match regardless of order or alias spelling",
          "[key_event_parser]") {
    CHECK(ParseKeySpec("shift+ctrl+tab") == ParseKeySpec("ctrl+shift+tab"));
    CHECK(Has(ParseKeySpec("shift+ctrl+tab"), Event::Special("\x1b[27;6;9~")));
    CHECK(ParseKeySpec("control+tab") == ParseKeySpec("ctrl+tab"));
    CHECK(Has(ParseKeySpec("control+tab"), Event::Special("\x1b[27;5;9~")));
}

// The bug: the arrow dispatch only tested ctrl/alt, so "shift+up" parsed to
// the same candidate list as plain "up" -- a configured shift+up binding
// fired on every ArrowUp, not just a shift-held one.
TEST_CASE("ParseKeySpec: shift+arrow is distinct from a plain arrow",
          "[key_event_parser]") {
    CHECK_FALSE(Has(ParseKeySpec("shift+up"), Event::ArrowUp));
    CHECK(Has(ParseKeySpec("shift+up"), Event::Special("\x1b[1;2A")));
    CHECK_FALSE(Has(ParseKeySpec("shift+down"), Event::ArrowDown));
    CHECK(Has(ParseKeySpec("shift+down"), Event::Special("\x1b[1;2B")));
    // ctrl+shift+arrow is likewise distinct from plain ctrl+arrow.
    CHECK_FALSE(Has(ParseKeySpec("ctrl+shift+up"), Event::ArrowUpCtrl));
    CHECK(Has(ParseKeySpec("ctrl+shift+up"), Event::Special("\x1b[1;6A")));
}

// The bug: a digit's shifted form is a different, layout-dependent symbol
// (US shift+5 sends '%', not a distinguishable "shifted 5"), so there is no
// safe single encoding to guess -- but the old code silently collapsed
// "shift+5" onto plain Character("5") anyway, matching every bare "5" press.
TEST_CASE("ParseKeySpec: shift+digit has no guessed encoding, unlike shift+letter",
          "[key_event_parser]") {
    CHECK(ParseKeySpec("shift+5").empty());
    CHECK_FALSE(Has(ParseKeySpec("shift+5"), Event::Character("5")));
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

TEST_CASE("ParseKeySpec: option/opt/meta are aliases for alt (the macOS key)",
          "[key_event_parser]") {
    // Every spelling parses to the same events -- a user who writes
    // "option+down" in keybindings.json gets exactly what "alt+down" gives.
    for (const std::string& spec : {"alt+down", "option+down", "opt+down", "meta+down"}) {
        CAPTURE(spec);
        CHECK(Has(ParseKeySpec(spec), Event::Special("\x1b[1;3B")));
        CHECK(Has(ParseKeySpec(spec), Event::Special("\x1b\x1b[B")));
    }
    CHECK(Has(ParseKeySpec("option+j"), Event::AltJ));
    CHECK(Has(ParseKeySpec("meta+j"), Event::AltJ));
    CHECK(Has(ParseKeySpec("control+a"), Event::CtrlA));   // control -> ctrl
    CHECK(ParseKeySpec("hyper+j").empty());                // unknown stays unknown
}

#if defined(__APPLE__)
TEST_CASE("ParseKeySpec: alt bindings also match the macOS Option glyphs",
          "[key_event_parser]") {
    // With the terminal's "Use Option as Meta Key" off, option+g sends "©"
    // and friends; the glyph rides along as a second candidate so the
    // binding fires anyway (the ESC-prefixed encoding stays, first).
    CHECK(Has(ParseKeySpec("alt+g"), Event::AltG));
    CHECK(Has(ParseKeySpec("alt+g"), Event::Character("©")));
    CHECK(Has(ParseKeySpec("option+g"), Event::Character("©")));
    CHECK(Has(ParseKeySpec("alt+j"), Event::Character("∆")));
    CHECK(Has(ParseKeySpec("alt+a"), Event::Character("å")));
    CHECK(Has(ParseKeySpec("alt+1"), Event::Character("¡")));
    CHECK_FALSE(Has(ParseKeySpec("ctrl+g"), Event::Character("©")));
    CHECK_FALSE(Has(ParseKeySpec("g"), Event::Character("å")));
}
#endif

TEST_CASE("NormalizeKeySpec: alias spellings share one collision identity",
          "[key_event_parser]") {
    // keybindings.cpp's collision warnings compare these, so "option+down"
    // vs the default "alt+down" must be seen as the same physical key.
    CHECK(NormalizeKeySpec("alt+down") == "alt+down");
    CHECK(NormalizeKeySpec("option+down") == "alt+down");
    CHECK(NormalizeKeySpec("OPT+Down") == "alt+down");
    CHECK(NormalizeKeySpec("meta+a") == "alt+a");
    CHECK(NormalizeKeySpec("control+a") == "ctrl+a");
    CHECK(NormalizeKeySpec("shift+ctrl+tab") == "ctrl+shift+tab");   // order-insensitive
    CHECK(NormalizeKeySpec("ctrl+j") == "enter");                    // C0 alias, kept
    CHECK(NormalizeKeySpec("control+j") == "enter");                 // ...via the alias too
    CHECK(NormalizeKeySpec("hyper+q") == "hyper+q");                 // unknown: raw spelling
}

// The bug: NamedKeys() maps every one of these pairs to the identical
// ftxui::Event, but NormalizeKeySpec emitted the raw spelling verbatim --
// a user's "esc" vs. the default turn.cancel=escape never collided in the
// startup warning.
TEST_CASE("NormalizeKeySpec: base-key alias spellings share one collision identity",
          "[key_event_parser]") {
    CHECK(NormalizeKeySpec("esc") == NormalizeKeySpec("escape"));
    CHECK(NormalizeKeySpec("return") == NormalizeKeySpec("enter"));
    CHECK(NormalizeKeySpec("del") == NormalizeKeySpec("delete"));
    CHECK(NormalizeKeySpec("ins") == NormalizeKeySpec("insert"));
    CHECK(NormalizeKeySpec("pgup") == NormalizeKeySpec("pageup"));
    CHECK(NormalizeKeySpec("pgdn") == NormalizeKeySpec("pagedown"));
    // Canonicalizes to the long spelling, matching keybindings.cpp's own
    // defaults (e.g. turn.cancel=escape, chat.load_older=pageup).
    CHECK(NormalizeKeySpec("esc") == "escape");
    CHECK(NormalizeKeySpec("ctrl+pgup") == "ctrl+pageup");
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
