#pragma once

// llm policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

namespace minimal::llm {

// providers.json "timeouts" defaults (connect_ms comes from
// common::kDefaultConnectTimeout).
// request_ms: a whole non-streaming call. Generous for a slow model writing a
// long answer; a dead backend is reported in minutes, not never.
inline constexpr std::chrono::milliseconds kDefaultRequestTimeout = std::chrono::minutes(2);
// stream_idle_ms: silence between bytes once a stream is flowing. A healthy
// stream emits every few hundred ms; a minute of nothing is a stall.
inline constexpr std::chrono::milliseconds kDefaultStreamIdleTimeout = std::chrono::minutes(1);
// first_byte_ms: silence before the first byte. A local backend prefilling a
// prompt of hundreds of thousands of tokens legitimately says nothing for
// minutes; cancel still lands within about a second.
inline constexpr std::chrono::milliseconds kDefaultFirstByteTimeout = std::chrono::minutes(10);

// JSON nesting accepted from a model or backend before parsing (the parser
// recurses). Real payloads are single-digit deep; this is far above them and
// far below a stack overflow.
inline constexpr std::size_t kMaxJsonDepth = 512;

}  // namespace minimal::llm
