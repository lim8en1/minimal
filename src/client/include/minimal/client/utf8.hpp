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

#include <cstddef>
#include <string>

namespace minimal::client {

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

}  // namespace minimal::client
