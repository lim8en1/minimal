#pragma once

// commands-md policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>
#include <cstdint>

namespace minimal::commands_md {

// A source with no `timeout:`. Far past any generator doing its job (git log,
// a diff, a script), short enough that a hung one is reported the same
// sitting. Cancel ends a run sooner either way.
inline constexpr int kDefaultTimeoutSeconds = 600;

// A source with no `output_cap: {max:}`, in estimated tokens. Room for a large
// diff or log, while an accidental dump (a whole build log) stops at the
// confirm/fail prompt before it is spent.
inline constexpr std::size_t kDefaultCapTokens = 50000;

// stderr is only ever an error narrative, kept head+tail: a few dozen lines
// carry a chatty generator's final lines, where the failure usually is.
inline constexpr std::int64_t kStderrCapBytes = 4000;

// RLIMIT_CPU is a backstop for what the wall-clock deadline's group kill
// cannot reach (a descendant that left the process group keeps the inherited
// limit). It sits a margin PAST the deadline so the deadline fires first and
// the run reports "timed out" rather than dying of SIGXCPU. CPU time is summed
// over threads, so no fixed margin can guarantee that ordering for a generator
// running many threads.
inline constexpr std::int64_t kCpuBackstopGraceSeconds = 30;

// Command files include PROJECT-LOCAL content (a cloned repo), read at every
// startup and listing: past this a file is a broken command, skipped with a
// warning. A command file is a prompt plus frontmatter; 1 MiB is orders of
// magnitude above any real one.
inline constexpr std::size_t kMaxCommandFileBytes = std::size_t{1} << 20;

}  // namespace minimal::commands_md
