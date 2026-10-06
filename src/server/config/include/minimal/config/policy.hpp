#pragma once

// config policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::config {

// --- settings.json tools.* defaults (also LoopConfig's) ------------------------

// Tool calls from one assistant turn run concurrently up to this width: the
// common fan-out (a few reads, a grep) at once, without a turn monopolizing
// the machine. 1 reproduces sequential dispatch.
inline constexpr int kDefaultMaxParallelToolCalls = 4;

// One tool call's serialized input, and one result's text. Far past any real
// call or a useful result, small enough that a runaway model output or a
// misbehaving MCP server cannot ride into every later request until
// compaction.
inline constexpr std::int64_t kDefaultMaxToolInputBytes = 10 * common::kMiB;
inline constexpr std::int64_t kDefaultMaxToolResultBytes = 10 * common::kMiB;

// A result's client-only display payload (a diff, a table): past this it is
// dropped and the client shows the text. Room for a large diff; a display
// payload is never the record.
inline constexpr std::int64_t kDefaultMaxToolDisplayBytes = 128 * common::kKiB;

}  // namespace minimal::config
