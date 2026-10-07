#include "minimal/client/sdk/key_event_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

#include "minimal/client/sdk/utf8.hpp"   // ToLower

namespace minimal::client {
namespace {

using ftxui::Event;

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

#if defined(__APPLE__)
// macOS Option glyphs, US layout: with the terminal's "Use Option as Meta
// Key" setting OFF (the default), Option+letter/digit sends the glyph
// instead of an ESC-prefixed alt encoding -- option+g sends "©", option+j
// "∆", option+a "å". AltCandidates below adds the glyph as a second
// candidate for every alt+letter/alt+digit spec, so bindings fire without
// that terminal setting. The trade-off is claiming: while "alt+g" is
// bound, "©" is no longer typeable into the prompt (HandleGlobalKey sees
// every event first). Only letters/digits are covered -- shift+option
// produces a different, layout-dependent glyph set (alt+shift+letter
// already degrades to plain alt, so those bindings match it anyway), and
// non-US layouts remap the whole table (a German layout's plain ß key
// sends "ß", which an "alt+s" binding then claims -- rebind rather than
// fight it). Not built on other platforms: there Option/Alt is ESC-prefixed
// and these glyph bytes only arise from compose sequences the user meant.
const std::unordered_map<char, const char*>& OptionGlyphs() {
    static const std::unordered_map<char, const char*> table = {
        {'a', "å"}, {'b', "∫"}, {'c', "ç"}, {'d', "∂"}, {'e', "´"},
        {'f', "ƒ"}, {'g', "©"}, {'h', "˙"}, {'i', "ˆ"}, {'j', "∆"},
        {'k', "˚"}, {'l', "¬"}, {'m', "µ"}, {'n', "˜"}, {'o', "ø"},
        {'p', "π"}, {'q', "œ"}, {'r', "®"}, {'s', "ß"}, {'t', "†"},
        {'u', "¨"}, {'v', "√"}, {'w', "∑"}, {'x', "≈"}, {'y', "¥"},
        {'z', "Ω"},
        {'1', "¡"}, {'2', "™"}, {'3', "£"}, {'4', "¢"}, {'5', "∞"},
        {'6', "§"}, {'7', "¶"}, {'8', "•"}, {'9', "ª"}, {'0', "º"},
    };
    return table;
}
#endif

// Every encoding an alt+letter / alt+digit press might produce: the classic
// ESC-prefixed alt byte, plus (on Apple builds) the US-layout Option glyph
// for terminals without the Meta setting.
std::vector<Event> AltCandidates(char c) {
    std::vector<Event> out{AltLetter(c)};
#if defined(__APPLE__)
    if (const auto it = OptionGlyphs().find(c); it != OptionGlyphs().end()) {
        out.push_back(Event::Character(it->second));
    }
#endif
    return out;
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
constexpr std::size_t kFunctionKeyCount = 12;  // NOLINT(readability-magic-numbers): F1-F12
const std::array<Event, kFunctionKeyCount>& FunctionKeys() {
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
// Base-key spelling aliases: NamedKeys() above maps every pair here to the
// identical ftxui::Event, but without this table NormalizeKeySpec emitted
// the raw spelling verbatim -- a user's "esc" vs. the default
// turn.cancel=escape (keybindings.cpp's DefaultBindings, which already
// spells these out long-form) is a real collision over one physical key
// that the startup warning never saw. Canonicalizes to the long spelling,
// matching the defaults.
const std::unordered_map<std::string, std::string>& NamedKeyAliases() {
    static const std::unordered_map<std::string, std::string> table = {
        {"esc", "escape"},
        {"return", "enter"},
        {"del", "delete"},
        {"ins", "insert"},
        {"pgup", "pageup"},
        {"pgdn", "pagedown"},
    };
    return table;
}

const std::unordered_map<std::string, std::string>& CtrlLetterAliases() {
    static const std::unordered_map<std::string, std::string> table = {
        {"j", "enter"},      // 0x0A == Event::Return
        {"m", "enter"},      // 0x0D, uniformized to \n == Event::Return
        {"i", "tab"},        // 0x09 == Event::Tab
        {"h", "backspace"},  // 0x08, uniformized to DEL == Event::Backspace
    };
    return table;
}

enum class ModifierToken { kCtrl, kAlt, kShift, kUnknown };

// The one place "ctrl"/"control", "alt"/"option"/"opt"/"meta", and "shift"
// are recognized -- NormalizeKeySpec and ParseKeySpec used to each keep their
// own copy of this list, and a future alias added to one and not the other
// would silently desync collision detection from event matching.
ModifierToken ClassifyModifier(const std::string& token) {
    if (token == "ctrl" || token == "control") return ModifierToken::kCtrl;
    if (token == "alt" || token == "option" || token == "opt" || token == "meta") {
        return ModifierToken::kAlt;
    }
    if (token == "shift") return ModifierToken::kShift;
    return ModifierToken::kUnknown;
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
        std::string canon;
        switch (ClassifyModifier(parts[i])) {
            case ModifierToken::kCtrl: canon = "ctrl"; break;
            case ModifierToken::kAlt: canon = "alt"; break;
            case ModifierToken::kShift: canon = "shift"; break;
            case ModifierToken::kUnknown: return lower;   // unknown token: keep the raw spelling visible
        }
        // Repeated modifiers ("ctrl+ctrl+c") must collapse to one identity --
        // ParseKeySpec's bool flags are naturally idempotent, so a spec that
        // fights over the same physical key as a single-modifier spec must
        // produce the identical identity here too, or the collision detector
        // misses it.
        if (std::find(modifiers.begin(), modifiers.end(), canon) == modifiers.end()) {
            modifiers.push_back(canon);
        }
    }
    // A modified LETTER's shift-case survives as a DISTINCT Event only in
    // the plain (no ctrl, no alt) path below (ParseKeySpec's toupper).
    // Combined with ctrl and/or alt, CtrlLetter/AltCandidates/CtrlAltLetter
    // never consult shift at all -- "alt+shift+c" and "alt+c" (also
    // "ctrl+shift+c" and "ctrl+c") already parse to the identical Event.
    // Drop shift from the identity in exactly that case so the collision
    // detector sees them as the one physical key they really are, instead
    // of two, and warns.
    if (parts.back().size() == 1 && std::isalpha(static_cast<unsigned char>(parts.back()[0])) &&
        (std::find(modifiers.begin(), modifiers.end(), "ctrl") != modifiers.end() ||
         std::find(modifiers.begin(), modifiers.end(), "alt") != modifiers.end())) {
        modifiers.erase(std::remove(modifiers.begin(), modifiers.end(), "shift"),
                        modifiers.end());
    }
    std::sort(modifiers.begin(), modifiers.end());
    std::string out;
    for (const auto& m : modifiers) {
        out += m;
        out += '+';
    }
    // Merge base-key aliases (esc/escape, return/enter, ...) onto one
    // canonical spelling -- see NamedKeyAliases()'s comment.
    std::string base = parts.back();
    if (const auto it = NamedKeyAliases().find(base); it != NamedKeyAliases().end()) {
        base = it->second;
    }
    out += base;

    if (modifiers.size() == 1 && modifiers.front() == "ctrl") {
        if (const auto it = CtrlLetterAliases().find(base); it != CtrlLetterAliases().end()) {
            return it->second;
        }
    }
    return out;
}

std::vector<Event> ParseKeySpec(const std::string& spec) {
    const std::string lower = ToLower(spec);
    if (lower.empty()) return {};

    std::vector<std::string> parts = Split(lower, '+');
    if (parts.empty()) return {};
    const std::string base = parts.back();
    parts.pop_back();

    bool ctrl = false, alt = false, shift = false;
    for (const auto& modifier : parts) {
        switch (ClassifyModifier(modifier)) {
            case ModifierToken::kCtrl: ctrl = true; break;
            case ModifierToken::kAlt: alt = true; break;
            case ModifierToken::kShift: shift = true; break;
            case ModifierToken::kUnknown: return {};   // an unrecognized modifier is "no mapping", not a guess
        }
    }

    // Tab/BackTab: ambiguous multi-byte combos that predate this parser and
    // must keep byte-identical candidates -- no general rule derives these
    // (they are workarounds for what specific terminals actually send
    // without the kitty keyboard protocol / CSI-u). Keyed on the PARSED
    // modifier set, not the raw spec string: "shift+ctrl+tab" (reordered)
    // and "control+tab" (the alias every other modifier already accepts)
    // must match identically to "ctrl+shift+tab" and "ctrl+tab". shift+tab
    // ("backtab") also lists FTXUI's own Event::TabReverse, since legacy
    // terminals send the CSI Z sequence FTXUI already decodes to that
    // constant -- both candidates listed so a Match() succeeds on whichever
    // arrives.
    if (base == "tab") {
        if (alt) return {};   // alt+tab and combinations: no known encoding
        if (ctrl && shift) return {Event::Special("\x1b[27;6;9~")};
        if (ctrl) return {Event::Special("\x1b[27;5;9~")};
        if (shift) return {Event::TabReverse, Event::Special("\x1b[27;2;9~")};
        // unmodified "tab" falls through to the NamedKeys() lookup below.
    }

    // Arrow keys: alt+ generalizes the old alt+left/alt+right-only special
    // case (both common CSI-u-less encodings) to all four directions;
    // ctrl+/shift+ and plain reuse FTXUI's own Arrow*Ctrl / Arrow* constants
    // or, for shift, the same xterm CSI modifier-code convention FTXUI's own
    // Arrow*Ctrl constants use (mod = 1 + 4 for ctrl; 2 for shift alone; 6
    // for ctrl+shift -- confirmed against FTXUI's event.cpp, which defines
    // Event::ArrowUpCtrl as Event::Special("\x1B[1;5A")).
    if (base == "left" || base == "right" || base == "up" || base == "down") {
        const char csi = base == "left" ? 'D' : base == "right" ? 'C' : base == "up" ? 'A' : 'B';
        if (alt && !ctrl) {
            return {Event::Special(std::string("\x1b[1;3") + csi),
                    Event::Special(std::string("\x1b\x1b[") + csi)};
        }
        if (ctrl && !alt) {
            if (shift) return {Event::Special(std::string("\x1b[1;6") + csi)};
            if (base == "left") return {Event::ArrowLeftCtrl};
            if (base == "right") return {Event::ArrowRightCtrl};
            if (base == "up") return {Event::ArrowUpCtrl};
            return {Event::ArrowDownCtrl};
        }
        if (!ctrl && !alt) {
            if (shift) return {Event::Special(std::string("\x1b[1;2") + csi)};
            if (base == "left") return {Event::ArrowLeft};
            if (base == "right") return {Event::ArrowRight};
            if (base == "up") return {Event::ArrowUp};
            return {Event::ArrowDown};
        }
        return {};   // ctrl+alt+arrow: no known single-candidate encoding
    }

    // Enter/Return with a modifier: prompt_editor.cpp's newline-insert key
    // (default "prompt.newline": "alt+enter", see keybindings.cpp). Unlike
    // other named keys (unmodified only, below), Alt+Enter needs every
    // encoding a terminal might emit for it -- the classic ESC-prefixed CR
    // or LF, the kitty keyboard protocol's CSI 13;3u, and xterm
    // modifyOtherKeys' CSI 27;3;13~ -- PLUS the CSI 13;2u / 27;2;13~ pair an
    // actual Shift+Enter reports on those same protocols. Before this spec
    // existed, prompt_editor.cpp's hardcoded IsNewlineEvent treated both
    // gestures as "insert a newline" with no way to tell them apart or
    // rebind away from either; alt+enter here reproduces that leniency
    // exactly, so the default binding's behavior is unchanged. A user whose
    // terminal cannot distinguish macOS's plain-\r Option+Enter (see
    // key_event_parser.hpp / docs/CONFIGURATION.md) can instead rebind
    // prompt.newline to "shift+enter" alone (just its own two CSI-u
    // candidates, no ESC CR/LF fallback -- there is no legacy encoding for
    // bare Shift+Enter) or to any ordinary letter/digit spec entirely
    // unrelated to Enter, sidestepping the ambiguity rather than fighting
    // it.
    if (base == "enter" || base == "return") {
        if (alt && !ctrl && !shift) {
            return {Event::Character("\x1b\r"),      Event::Character("\x1b\n"),
                    Event::Special("\x1b[13;3u"),     Event::Special("\x1b[27;3;13~"),
                    Event::Special("\x1b[13;2u"),     Event::Special("\x1b[27;2;13~")};
        }
        if (shift && !ctrl && !alt) {
            return {Event::Special("\x1b[13;2u"), Event::Special("\x1b[27;2;13~")};
        }
        if (ctrl || (alt && shift)) return {};   // no known single encoding
        // unmodified "enter"/"return" falls through to NamedKeys() below.
    }

    // Named non-alphanumeric keys: unmodified only (ctrl/alt/shift combined
    // with e.g. tab or escape have no one universal encoding beyond the two
    // ctrl+tab cases, and enter/return's modified forms, handled above).
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
        if (n >= 1 && static_cast<std::size_t>(n) <= kFunctionKeyCount) {
        return {FunctionKeys()[static_cast<std::size_t>(n - 1)]};
    }
    }

    // A single letter, any modifier combination.
    if (base.size() == 1 && std::isalpha(static_cast<unsigned char>(base[0]))) {
        const char c = base[0];
        if (ctrl && alt) return {CtrlAltLetter(c)};
        if (ctrl) return {CtrlLetter(c)};
        if (alt) return AltCandidates(c);
        const char out = shift ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        return {Event::Character(std::string(1, out))};
    }

    // A single digit: alt+digit (ESC + digit) and plain digit are
    // well-defined; ctrl+digit and shift+digit have no single standard byte
    // encoding across terminals/layouts (unlike a letter, where shift just
    // changes case, a digit's shifted form is a different, layout-dependent
    // symbol -- US shift+5 sends '%', not a distinguishable "shifted 5"), so
    // both are left unmapped rather than guessed. Without the shift check, a
    // "shift+5" binding silently collapsed onto plain Character("5").
    if (base.size() == 1 && std::isdigit(static_cast<unsigned char>(base[0]))) {
        if (alt && !ctrl) return AltCandidates(base[0]);
        if (!ctrl && !alt && !shift) return {Event::Character(base)};
        return {};
    }

    return {};
}

// True iff `event` is byte-identical to one of AltCandidates' macOS
// Option-glyph candidates (OptionGlyphs above) -- i.e. a paste of that
// literal glyph is indistinguishable from the key actually being pressed.
// Always false on non-Apple builds, where AltCandidates never adds a glyph
// candidate. Used only to gate HandleGlobalKey mid-paste
// (keybinding_dispatch.cpp); never consulted by ParseKeySpec/event matching
// itself.
#if defined(__APPLE__)
bool IsOptionGlyphEvent(const Event& event) {
    for (const auto& [letter, glyph] : OptionGlyphs()) {
        (void)letter;
        if (event == Event::Character(glyph)) return true;
    }
    return false;
}
#else
bool IsOptionGlyphEvent(const Event&) { return false; }
#endif

}  // namespace minimal::client
