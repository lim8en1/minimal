#pragma once

// llm policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

#include "minimal/common/units.hpp"

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

// --- provider adapters ---------------------------------------------------------

// The head of an error body kept and parsed for a message. The body can be as
// large as the HTTP client's caps; parsing all of it to extract one string
// stalls on a hostile or absurd body, and a drip-fed error body must not hold
// the transfer open. Error JSON is a few hundred bytes.
inline constexpr std::size_t kMaxErrorBodyBytes = 64 * common::kKiB;

// Everything one stream may accumulate (text, thinking, tool-input deltas):
// the SSE parser bounds lines, nothing else bounds the total, and a server
// drip-feeding under the idle timeout would grow it forever. Many times a
// legitimate million-token response.
inline constexpr std::size_t kMaxStreamContentBytes = 64 * common::kMiB;
// Content blocks one response may index: the index comes off the wire, and
// sizing a vector by it is an allocation the wire controls. No real message
// comes close.
inline constexpr std::size_t kMaxStreamBlocks = 4096;

// A Retry-After past this is ignored (the normal backoff applies): above any
// real rate-limit window, and a server must not park a turn for hours.
inline constexpr std::chrono::milliseconds kMaxRetryAfter = std::chrono::minutes(10);

// GET <base>/models for model discovery: off the chat path, serving users
// (--model resolution, the /model picker), so a hung backend must not wedge a
// caller for a chat-length timeout. Local servers answer in milliseconds,
// remote catalogs in well under this.
inline constexpr std::chrono::milliseconds kListModelsTimeout = std::chrono::seconds(10);

}  // namespace minimal::llm
