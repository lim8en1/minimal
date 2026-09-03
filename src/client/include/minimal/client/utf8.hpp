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

// Counts CODEPOINTS (lead bytes), not raw bytes: an approximation of terminal
// column width that treats every codepoint as one column. Not a full wcwidth
// (a wide CJK glyph still counts as 1, so it under-measures those), but a
// vast improvement over counting UTF-8 BYTES, which over-counts every
// non-ASCII character 2-4x -- the bug this replaced (tab_bar.cpp truncating
// and budgeting tab labels by `std::string::size()`).
inline std::size_t Utf8ColumnWidth(std::string_view text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;  // not a continuation byte
    }
    return count;
}

// Truncates to at most `max_columns` display columns (see Utf8ColumnWidth
// above), appending an ellipsis when truncation happens. Never splits a
// codepoint. One column of `max_columns` is reserved for the ellipsis itself,
// so the result's own column width never exceeds `max_columns`.
inline std::string TruncateUtf8Columns(const std::string& text, std::size_t max_columns) {
    if (Utf8ColumnWidth(text) <= max_columns) return text;
    if (max_columns == 0) return "";
    const std::size_t keep = max_columns - 1;
    std::size_t seen = 0;
    std::size_t cut = text.size();
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (seen == keep) {
                cut = i;
                break;
            }
            ++seen;
        }
    }
    return text.substr(0, cut) + "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
}

}  // namespace minimal::client
