#include "minimal/client/key_event_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace minimal::client {
namespace {

using ftxui::Event;

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "ctrl+shift+tab" -> {"ctrl", "shift", "tab"}. No empty tokens: "a++b" or a
// leading/trailing '+' yields no match downstream rather than a crash.
std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (c == sep) {
            if (!current.empty()) out.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

// FTXUI's own encoding, read off event.cpp (Event::==  compares only the raw
// byte string, never the Character-vs-Special tag, so building everything
// through Special() here is exactly equivalent to whichever constructor
// FTXUI's own per-letter constants happen to use).
Event CtrlLetter(char lower_letter) {
    return Event::Special(std::string(1, static_cast<char>(lower_letter - 'a' + 1)));
}
Event AltLetter(char lower_letter) {
    return Event::Special(std::string("\x1b") + lower_letter);
}
Event CtrlAltLetter(char lower_letter) {
    return Event::Special(std::string("\x1b") +
                          static_cast<char>(lower_letter - 'a' + 1));
}

const std::unordered_map<std::string, Event>& NamedKeys() {
    static const std::unordered_map<std::string, Event> table = {
        {"tab", Event::Tab},
        {"escape", Event::Escape},
        {"esc", Event::Escape},
        {"backspace", Event::Backspace},
        {"delete", Event::Delete},
        {"del", Event::Delete},
        {"insert", Event::Insert},
        {"ins", Event::Insert},
        {"home", Event::Home},
        {"end", Event::End},
        {"pageup", Event::PageUp},
        {"pgup", Event::PageUp},
        {"pagedown", Event::PageDown},
        {"pgdn", Event::PageDown},
        {"return", Event::Return},
        {"enter", Event::Return},
        {"space", Event::Character(" ")},
    };
    return table;
}

// index 0 = F1 .. index 11 = F12.
const std::array<Event, 12>& FunctionKeys() {
    static const std::array<Event, 12> table = {
        Event::F1, Event::F2, Event::F3,  Event::F4,  Event::F5,  Event::F6,
        Event::F7, Event::F8, Event::F9,  Event::F10, Event::F11, Event::F12,
    };
    return table;
}

// The C0 control-code aliases: a bare ctrl+letter shares its encoding byte
// with another key's whole event (FTXUI uniformizes \r and 0x08 on input, so
// every row below is byte-identical to its target, not merely similar). The
// ctrl+letter MAPPINGS themselves must stay -- real terminals send these
// bytes and removing them breaks working bindings -- but a user who binds
// "ctrl+j" while something else owns Enter gets a binding that silently
// never fires: the alias table is what keybindings.cpp's collision warning
// consults to make that visible at startup.
const std::unordered_map<std::string, std::string>& CtrlLetterAliases() {
    static const std::unordered_map<std::string, std::string> table = {
        {"j", "enter"},      // 0x0A == Event::Return
        {"m", "enter"},      // 0x0D, uniformized to \n == Event::Return
        {"i", "tab"},        // 0x09 == Event::Tab
        {"h", "backspace"},  // 0x08, uniformized to DEL == Event::Backspace
    };
    return table;
}

}  // namespace

// The collision identity of a key spec: modifiers are canonicalized
// ("control" -> "ctrl", "option"/"opt"/"meta" -> "alt" -- the same aliases
// ParseKeySpec accepts) and sorted, so spellings that parse to identical
// events -- "alt+down" vs "option+down", "shift+ctrl+tab" vs
// "ctrl+shift+tab" -- share one identity and keybindings.cpp's collision
// warnings see them as the fight over one physical key that they are. On
// top of that, a bare "ctrl+<letter>" whose C0
// control byte doubles as another key's WHOLE event (ctrl+j/m -> enter,
// ctrl+i -> tab, ctrl+h -> backspace; FTXUI uniformizes \r and 0x08 to \n
// and DEL on input, so these are byte-identical matches, not near misses)
// normalizes to that key's name. Everything informational only: used for
// startup warnings, never for event matching (ParseKeySpec above owns
// that).
std::string NormalizeKeySpec(const std::string& spec) {
    const std::string lower = ToLower(spec);
    const std::vector<std::string> parts = Split(lower, '+');
    if (parts.empty()) return lower;

    // Canonical modifier spellings, sorted so the identity is
    // order-insensitive the way ParseKeySpec is.
    std::vector<std::string> modifiers;
    modifiers.reserve(parts.size() - 1);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& m = parts[i];
        if (m == "ctrl" || m == "control") {
            modifiers.push_back("ctrl");
        } else if (m == "alt" || m == "option" || m == "opt" || m == "meta") {
            modifiers.push_back("alt");
        } else if (m == "shift") {
            modifiers.push_back("shift");
        } else {
            return lower;   // unknown token: keep the raw spelling visible
        }
    }
    std::sort(modifiers.begin(), modifiers.end());
    std::string out;
    for (const auto& m : modifiers) {
        out += m;
        out += '+';
    }
    out += parts.back();

    if (modifiers.size() == 1 && modifiers.front() == "ctrl") {
        if (const auto it = CtrlLetterAliases().find(parts.back());
            it != CtrlLetterAliases().end()) {
            return it->second;
        }
    }
    return out;
}

std::vector<Event> ParseKeySpec(const std::string& spec) {
    const std::string lower = ToLower(spec);
    if (lower.empty()) return {};

    // Ambiguous multi-byte combos that predate this parser and must keep
    // byte-identical candidates: no general rule derives these (they are
    // workarounds for what specific terminals actually send without the
    // kitty keyboard protocol / CSI-u).
    if (lower == "ctrl+tab") return {Event::Special("\x1b[27;5;9~")};
    if (lower == "ctrl+shift+tab") return {Event::Special("\x1b[27;6;9~")};
    // shift+tab ("backtab"): terminals send the legacy CSI Z sequence; CSI-u
    // terminals send the explicit modifier form. Both listed so a Match()
    // succeeds on whichever arrives (the same dual-candidate rule as
    // ctrl+tab above).
    if (lower == "shift+tab") {
        return {Event::TabReverse, Event::Special("\x1b[27;2;9~")};
    }

    std::vector<std::string> parts = Split(lower, '+');
    if (parts.empty()) return {};
    const std::string base = parts.back();
    parts.pop_back();

    bool ctrl = false, alt = false, shift = false;
    for (const auto& modifier : parts) {
        if (modifier == "ctrl" || modifier == "control") {
            ctrl = true;
        } else if (modifier == "alt" || modifier == "option" || modifier == "opt" ||
                   modifier == "meta") {
            alt = true;
        } else if (modifier == "shift") {
            shift = true;
        } else {
            return {};   // an unrecognized modifier is "no mapping", not a guess
        }
    }

    // Arrow keys: alt+ generalizes the old alt+left/alt+right-only special
    // case (both common CSI-u-less encodings) to all four directions;
    // ctrl+ and plain reuse FTXUI's own Arrow*Ctrl / Arrow* constants.
    if (base == "left" || base == "right" || base == "up" || base == "down") {
        const char csi = base == "left" ? 'D' : base == "right" ? 'C' : base == "up" ? 'A' : 'B';
        if (alt && !ctrl) {
            return {Event::Special(std::string("\x1b[1;3") + csi),
                    Event::Special(std::string("\x1b\x1b[") + csi)};
        }
        if (ctrl && !alt) {
            if (base == "left") return {Event::ArrowLeftCtrl};
            if (base == "right") return {Event::ArrowRightCtrl};
            if (base == "up") return {Event::ArrowUpCtrl};
            return {Event::ArrowDownCtrl};
        }
        if (!ctrl && !alt) {
            if (base == "left") return {Event::ArrowLeft};
            if (base == "right") return {Event::ArrowRight};
            if (base == "up") return {Event::ArrowUp};
            return {Event::ArrowDown};
        }
        return {};   // ctrl+alt+arrow: no known single-candidate encoding
    }

    // Named non-alphanumeric keys: unmodified only (ctrl/alt/shift combined
    // with e.g. tab or escape have no one universal encoding beyond the two
    // ctrl+tab cases handled above).
    if (!ctrl && !alt && !shift) {
        if (const auto it = NamedKeys().find(base); it != NamedKeys().end()) return {it->second};
    }

    // F1..F12.
    if (!ctrl && !alt && !shift && base.size() >= 2 && base.front() == 'f' &&
        std::all_of(base.begin() + 1, base.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        int n = 0;
        try {
            n = std::stoi(base.substr(1));
        } catch (const std::exception&) {
            // The digit check above only proves DECIMAL DIGITS, not range:
            // "f99999999999999" reached std::stoi and threw out_of_range --
            // straight out of a user's keybindings.json to a startup crash.
            // Out of range is just "no mapping", like every other unparsable
            // spec here.
            return {};
        }
        if (n >= 1 && n <= 12) return {FunctionKeys()[static_cast<std::size_t>(n - 1)]};
    }

    // A single letter, any modifier combination.
    if (base.size() == 1 && std::isalpha(static_cast<unsigned char>(base[0]))) {
        const char c = base[0];
        if (ctrl && alt) return {CtrlAltLetter(c)};
        if (ctrl) return {CtrlLetter(c)};
        if (alt) return {AltLetter(c)};
        const char out = shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        return {Event::Character(std::string(1, out))};
    }

    // A single digit: alt+digit (ESC + digit) and plain digit are
    // well-defined; ctrl+digit has no single standard byte encoding across
    // terminals, so it is left unmapped rather than guessed.
    if (base.size() == 1 && std::isdigit(static_cast<unsigned char>(base[0]))) {
        if (alt && !ctrl) return {Event::Special(std::string("\x1b") + base)};
        if (!ctrl && !alt) return {Event::Character(base)};
        return {};
    }

    return {};
}

}  // namespace minimal::client
