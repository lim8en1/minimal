#pragma once

// UTF-8-aware truncation, in a header because it is a correctness rule rather
// than a formatting nicety and it needs testing on its own.
//
// The concrete case: the headless client derives a session name from the first
// 60 bytes of the prompt. `CreateSessionRequest.name` is a proto3 `string`, so
// the SERVER's parser rejects invalid UTF-8 while the client's serializer only
// logs a warning -- a plain substr therefore turned any non-ASCII prompt whose
// 60th byte fell inside a multi-byte sequence into an opaque "could not create
// a session" parse error, with nothing pointing at the name.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include <ftxui/screen/string.hpp>

#include "minimal/common/utf8.hpp"

namespace minimal::client {

// ASCII-only lowercasing for case-insensitive filter matching (the pickers'
// session/preset names and descriptions). Not full Unicode case-folding --
// nothing in this codebase needs that yet.
inline std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Byte-cut helpers live in common (shared with server and plugins); these are
// the client's historical names for them.
inline std::string TruncateUtf8(const std::string& text, std::size_t max_bytes) {
    return common::TruncateUtf8Bytes(text, max_bytes);
}
using common::Utf8CeilToBoundary;

// Terminal display columns for one code point -- FTXUI's own table, the one
// its renderer lays glyphs out by. The hand-kept range list this replaced
// agreed with it for CJK but not for most emoji (U+1F680 transport, U+1FA70,
// regional indicators) or zero-width joiners/variation selectors, so every
// measurement built on it -- the prompt's wrap and caret column, tab-bar
// budgets -- drifted from what was actually drawn on such text. Negative
// (control characters) reads as 0: they draw nothing.
inline std::size_t Utf8CharColumnWidth(char32_t cp) {
    if (cp < 0x80) return cp < 0x20 || cp == 0x7F ? (cp == U'\t' ? 1 : 0) : 1;
    char buf[4];
    std::size_t n = 0;
    if (cp < 0x800) {
        buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = static_cast<char>(0xF0 | (cp >> 18));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
    const int width = ftxui::string_width(std::string_view(buf, n));
    return width > 0 ? static_cast<std::size_t>(width) : 0;
}

// Decodes the UTF-8 code point at text[*i] and advances *i past it. Malformed
// sequences (a truncated or non-continuation tail) are consumed one byte at a
// time as U+FFFD: measurement must never walk off the end of a bad label, and
// a width-1 approximation for each stray byte is the same treatment the old
// byte-counting code effectively gave them.
inline char32_t Utf8Decode(std::string_view text, std::size_t& i) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    char32_t cp = 0;
    if (lead < 0x80) {
        length = 1;
        cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07;
    } else {
        ++i;   // stray continuation byte
        return 0xFFFD;
    }
    if (i + length > text.size()) {
        ++i;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char cont = static_cast<unsigned char>(text[i + k]);
        if ((cont & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    i += length;
    return cp;
}

// Drops C0 controls (0x00-0x1F), DEL (0x7F), and C1 controls (0x80-0x9F) --
// an untrusted display string (a server-titler-generated session name, e.g.)
// must never carry a raw ESC or newline into a terminal write: either breaks
// a table's layout or, for ESC specifically, is a terminal escape-sequence
// injection. Leaves everything else, including wide/multi-byte text,
// byte-exact.
inline std::string StripControlChars(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const char32_t cp = Utf8Decode(text, i);
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) continue;
        out.append(text.substr(start, i - start));
    }
    return out;
}

// Repairs invalid UTF-8 byte-for-byte: each malformed sequence Utf8Decode
// would report as U+FFFD is re-encoded as the real U+FFFD replacement
// character (valid input, including a genuine pre-existing U+FFFD, passes
// through byte-identical). Used at the one place outside a JSON/proto
// serializer that takes a raw OS string (argv) and must guarantee valid
// UTF-8 before it reaches a proto string field -- unlike nlohmann::json's
// own dump()-time replace mode (used server-side), there is no serializer in
// the way here to do it implicitly.
inline std::string SanitizeUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const char32_t cp = Utf8Decode(text, i);
        out.append(cp == 0xFFFD ? "\xEF\xBF\xBD" : std::string(text.substr(start, i - start)));
    }
    return out;
}

// Decodes the LAST code point of text and returns the byte length of its
// UTF-8 sequence (0 when text is empty). Used by Utf8PopGrapheme.
inline std::size_t Utf8LastCodepointBytes(std::string_view text) {
    if (text.empty()) return 0;
    std::size_t end = text.size() - 1;
    // Back up over continuation bytes to the lead byte.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.size() - end;
}

// Pops one USER-PERCEIVED character for backspace: a code point, plus any
// glue it was part of. A plain code-point pop splits emoji composed with a
// zero-width joiner (families, skin-tone thumbs-up) or a variation selector
// into visible fragments -- the delete key then needs several presses per
// glyph and the intermediate states render as bare components. Heuristic,
// not full UAX #29: handles ZWJ sequences, U+FE0E/U+FE0F selectors, and
// combining marks (which contribute no column -- see Utf8CharColumnWidth).
inline std::string_view Utf8PopGrapheme(std::string_view text) {
    const auto is_glue = [](char32_t cp) {
        return cp == 0x200D /* ZWJ */ || cp == 0xFE0E || cp == 0xFE0F ||
               (cp >= 0x0300 && cp <= 0x036F) /* combining marks */;
    };
    std::size_t len = Utf8LastCodepointBytes(text);
    while (len > 0) {
        // Decode the codepoint about to be removed: a ZWJ or a variation
        // selector/combining mark is glue for what PRECEDES it, so removing
        // one of those always continues the pop unconditionally -- checking
        // the codepoint newly exposed AFTER removal (the old bug) tests the
        // wrong side of the glue and stops one codepoint too early.
        std::size_t i = text.size() - len;
        const char32_t cp = Utf8Decode(text, i);
        text.remove_suffix(len);
        bool continue_popping = is_glue(cp);
        if (!continue_popping) {
            // `cp` was an ordinary base character. If a ZWJ now sits at the
            // new end, `cp` was the right-hand half of a ZWJ join -- the
            // cluster continues through the ZWJ (and, via this same check
            // next iteration, through whatever it joins in turn).
            const std::size_t prev = Utf8LastCodepointBytes(text);
            if (prev > 0) {
                std::size_t j = text.size() - prev;
                continue_popping = Utf8Decode(text, j) == 0x200D;
            }
        }
        if (!continue_popping) break;
        len = Utf8LastCodepointBytes(text);
    }
    return text;
}

// Counts DISPLAY COLUMNS, not raw bytes and not codepoints: a wide CJK/emoji
// code point is 2 columns, a combining mark 0, everything else 1 (see
// Utf8CharColumnWidth above).
inline std::size_t Utf8ColumnWidth(std::string_view text) {
    std::size_t columns = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        columns += Utf8CharColumnWidth(Utf8Decode(text, i));
    }
    return columns;
}

// Truncates to at most `max_columns` display columns (see Utf8ColumnWidth
// above), appending an ellipsis when truncation happens. Never splits a
// codepoint: the cut lands on a lead byte, and because a wide character
// contributes its full 2 columns atomically, one is never half-kept at the
// limit. One column of `max_columns` is reserved for the ellipsis itself, so
// the result's own column width never exceeds `max_columns`.
inline std::string TruncateUtf8Columns(const std::string& text, std::size_t max_columns) {
    if (Utf8ColumnWidth(text) <= max_columns) return text;
    if (max_columns == 0) return "";
    const std::size_t keep = max_columns - 1;
    std::size_t seen = 0;
    std::size_t cut = text.size();
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const std::size_t width = Utf8CharColumnWidth(Utf8Decode(text, i));
        if (seen + width > keep) {
            cut = start;
            break;
        }
        seen += width;
    }
    return text.substr(0, cut) + "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
}

// Tabs to spaces at 4-column stops, for text drawn through ftxui::text(),
// which draws a tab as nothing. Columns restart at each newline; a UTF-8
// sequence advances one column (continuation bytes do not count -- the
// byte-counting copies this replaced put stops late after any non-ASCII).
// `strip_cr` drops carriage returns, which text() would draw as junk.
inline std::string ExpandTabs(std::string_view text, bool strip_cr = true) {
    if (text.find('\t') == std::string_view::npos &&
        (!strip_cr || text.find('\r') == std::string_view::npos)) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());
    std::size_t column = 0;
    for (const char c : text) {
        if (c == '\t') {
            const std::size_t pad = 4 - (column % 4);
            out.append(pad, ' ');
            column += pad;
        } else if (c == '\n') {
            out += c;
            column = 0;
        } else if (c == '\r' && strip_cr) {
            continue;
        } else {
            out += c;
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++column;
        }
    }
    return out;
}

}  // namespace minimal::client
