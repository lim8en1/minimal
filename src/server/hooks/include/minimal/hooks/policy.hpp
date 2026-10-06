#pragma once

// hooks policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::hooks {

// A hook's output kept in memory: only its first non-empty line ever becomes
// a block reason, but a hook piping a build log must not grow a string on the
// caller's thread. Past this the output is still drained (head and tail kept),
// so the child never blocks writing.
inline constexpr std::int64_t kMaxHookCapturedBytes = common::kMiB;

// A block reason (the first line of a vetoing hook's output) shown to the
// model and the user: a sentence or two.
inline constexpr std::size_t kMaxHookBlockReasonBytes = 500;

// Budget for an uncancellable hook run configured with no budget (0). The
// async hook worker is one thread, so a wedged script would block every later
// hook forever; this is long enough for any real hook, finite.
inline constexpr std::chrono::milliseconds kUncancellableHookFloor = std::chrono::minutes(1);

}  // namespace minimal::hooks
