#pragma once

// net policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::net {

// A buffered Post() body (a completion): orders of magnitude above any
// legitimate completion, orders below anything that could exhaust memory when
// a proxy streams an endless or mislabelled body.
inline constexpr std::size_t kMaxPostBodyBytes = 64 * common::kMiB;

// A buffered GET body (model catalogs, rate tables): catalogs are hundreds of
// KiB at most.
inline constexpr std::size_t kMaxResponseBodyBytes = 8 * common::kMiB;

// Response headers: status, content type, request ids, rate-limit buckets --
// kilobytes; past this a response is malformed.
inline constexpr std::size_t kMaxHeaderBytes = 64 * common::kKiB;

// One SSE line or frame still being assembled. Deltas are small JSON objects;
// past this the stream is broken (no newline, or a frame that never ends) and
// fails instead of growing until the idle timeout.
inline constexpr std::size_t kMaxSsePendingBytes = common::kMiB;

}  // namespace minimal::net
