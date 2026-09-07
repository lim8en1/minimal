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

// Never returns more than `max_bytes`, and never ends mid-character: a sequence
// straddling the limit is dropped whole.
inline std::string TruncateUtf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t end = max_bytes;
    // Continuation bytes are 10xxxxxx. Back up to the lead byte of the character
    // straddling the limit, then drop it.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

// Terminal display columns for one code point, per the ranges East Asian
// Width marks Wide/Fullwidth and the common emoji planes. Not a full wcwidth
// (no ambient-charset awareness, no unassigned-code-point subtleties) but it
// covers what actually overflows a tab budget: CJK text and emoji count 2,
// not 1, which is the bug this replaced (codepoint counting under-measured
// those labels 2x, so "budgeted" tab bars still clipped or wrapped).
inline std::size_t Utf8CharColumnWidth(char32_t cp) {
    // Combining marks occupy no column of their own (they modify the
    // preceding one).
    if (cp >= 0x0300 && cp <= 0x036F) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo leading consonants
        (cp >= 0x2E80 && cp <= 0xA4CF) ||    // CJK radicals through Yijing hexagrams
        (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK compatibility ideographs
        (cp >= 0xFE30 && cp <= 0xFE4F) ||    // CJK compatibility forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||    // fullwidth ASCII/forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||    // fullwidth symbols
        (cp >= 0x1F300 && cp <= 0x1F64F) ||  // emoji (misc symbols and pictographs)
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||  // emoji (supplemental symbols)
        (cp >= 0x20000 && cp <= 0x3FFFD)) {  // CJK extension planes B+
        return 2;
    }
    return 1;
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
    std::size_t len = Utf8LastCodepointBytes(text);
    while (len > 0) {
        text.remove_suffix(len);
        // Continue popping while the new tail is glue that bound what we
        // just removed to what precedes it.
        const std::size_t prev = Utf8LastCodepointBytes(text);
        if (prev == 0) break;
        const std::size_t at = text.size() - prev;
        char32_t cp = 0;
        std::size_t i = at;
        cp = Utf8Decode(text, i);
        const bool is_glue = cp == 0x200D /* ZWJ */ || cp == 0xFE0E || cp == 0xFE0F ||
                             (cp >= 0x0300 && cp <= 0x036F) /* combining marks */;
        if (!is_glue) break;
        len = prev;
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

}  // namespace minimal::client
