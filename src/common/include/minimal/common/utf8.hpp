#pragma once

// UTF-8-aware truncation for server-side text that reaches a model or a
// proto3 string: a byte cut could split a multi-byte sequence and render
// U+FFFD (or fail a proto parse). The client keeps its own copy in
// minimal/client/utf8.hpp; this is the server/plugin one, so plugins stop
// hand-rolling the same loop.

#include <cstddef>
#include <string>
#include <string_view>

namespace minimal::common {

// At most `max_bytes`, never ending mid-character: a sequence straddling the
// limit is dropped whole.
inline std::string TruncateUtf8Bytes(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
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

}  // namespace minimal::common
