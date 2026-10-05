#pragma once

// UTF-8 helpers for text that reaches a model, a terminal or a proto3
// string: a byte cut could split a multi-byte sequence and render U+FFFD (or
// fail a proto parse). Shared by server, client and plugins.

#include <cstddef>
#include <string>
#include <string_view>

namespace minimal::common {

namespace utf8_detail {
inline bool IsContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}  // namespace utf8_detail

// The largest index <= pos that does not split a character, for cutting the
// END of a kept head. Walks back at most 3 continuation bytes (the longest a
// well-formed sequence has); past that the text is malformed and `pos` stands.
inline std::size_t Utf8FloorToBoundary(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return text.size();
    for (std::size_t back = 0; back <= 3 && back <= pos; ++back) {
        if (!utf8_detail::IsContinuation(text[pos - back])) return pos - back;
    }
    return pos;
}

// The smallest index >= pos that does not split a character, for cutting the
// START of a kept tail. Same 3-byte bound as the floor.
inline std::size_t Utf8CeilToBoundary(std::string_view text, std::size_t pos) {
    for (std::size_t ahead = 0; ahead <= 3; ++ahead) {
        if (pos + ahead >= text.size()) return text.size();
        if (!utf8_detail::IsContinuation(text[pos + ahead])) return pos + ahead;
    }
    return pos;
}

// At most `max_bytes`, never ending mid-character: a sequence straddling the
// limit is dropped whole.
inline std::string TruncateUtf8Bytes(std::string_view text, std::size_t max_bytes) {
    return std::string(text.substr(0, Utf8FloorToBoundary(text, max_bytes)));
}

// The first `max_chars` code points. Invalid lead bytes count as one
// character each, so malformed input still terminates and never over-reads.
inline std::string TruncateCodePoints(std::string_view text, std::size_t max_chars) {
    std::size_t chars = 0, i = 0;
    while (i < text.size() && chars < max_chars) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                          : (c & 0xF0) == 0xE0 ? 3
                          : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > text.size()) len = text.size() - i;
        i += len;
        ++chars;
    }
    return std::string(text.substr(0, i));
}

namespace utf8_detail {

// The well-formed sequence starting at text[i] (Unicode Table 3-7: no
// overlong forms, no surrogates, nothing above U+10FFFF). Returns its length
// (1..4), or 0 when ill-formed; then `prefix` is how many bytes formed a valid
// prefix (the maximal subpart one U+FFFD replaces) and `cut` whether that
// prefix was cut by the end of the text rather than by a bad byte.
inline std::size_t SequenceAt(std::string_view text, std::size_t i, std::size_t& prefix,
                              bool& cut) {
    prefix = 1;
    cut = false;
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) return 1;
    std::size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;   // the second byte's range
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c == 0xE0) {
        len = 3, lo = 0xA0;
    } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
        len = 3;
    } else if (c == 0xED) {
        len = 3, hi = 0x9F;
    } else if (c == 0xF0) {
        len = 4, lo = 0x90;
    } else if (c >= 0xF1 && c <= 0xF3) {
        len = 4;
    } else if (c == 0xF4) {
        len = 4, hi = 0x8F;
    } else {
        return 0;   // a continuation byte, C0/C1, or F5..FF
    }
    for (std::size_t k = 1; k < len; ++k) {
        if (i + k >= text.size()) {
            cut = true;
            return 0;
        }
        const unsigned char b = static_cast<unsigned char>(text[i + k]);
        if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) return 0;
        prefix = k + 1;
    }
    return len;
}

}  // namespace utf8_detail

// Valid UTF-8, for text headed into a proto3 `string` field (which a peer's
// parser rejects outright when ill-formed -- the whole message is lost). Each
// ill-formed subsequence becomes one U+FFFD; valid input comes back unchanged.
inline std::string ScrubUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t prefix = 0;
        bool cut = false;
        if (const std::size_t n = utf8_detail::SequenceAt(text, i, prefix, cut)) {
            out.append(text.substr(i, n));
            i += n;
        } else {
            out.append("\xEF\xBF\xBD");
            i += prefix;
        }
    }
    return out;
}

// How many trailing bytes are the start of a character the text was cut in
// the middle of (0..3): what a chunked stream holds back for its next chunk,
// so a character split between two reads is not turned into U+FFFD.
inline std::size_t IncompleteUtf8Tail(std::string_view text) {
    const std::size_t floor = text.size() > 3 ? text.size() - 3 : 0;
    for (std::size_t start = text.size(); start-- > floor;) {
        const unsigned char c = static_cast<unsigned char>(text[start]);
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte: keep looking for its lead
        std::size_t prefix = 0;
        bool cut = false;
        utf8_detail::SequenceAt(text, start, prefix, cut);
        return cut && prefix == text.size() - start ? text.size() - start : 0;
    }
    return 0;
}

}  // namespace minimal::common
