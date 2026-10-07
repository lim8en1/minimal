#pragma once

// tools-bash policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::tools_bash {

// --- plugins.json tools-bash.config defaults ---------------------------------------

// A call's wall-clock budget when the model gives none: a test run or a build
// step fits; a hang is reported in minutes, not never.
inline constexpr std::chrono::milliseconds kDefaultTimeout = std::chrono::minutes(2);
// Ceiling on a model-supplied timeout: a build is not `ls`, but the deadline
// is the only hard bound on a session worker thread.
inline constexpr std::chrono::milliseconds kDefaultMaxTimeout = std::chrono::minutes(10);
// A background task never holds the worker, but nothing runs forever on the
// operator's machine: bounded separately and generously; a task given no
// timeout gets this.
inline constexpr std::chrono::milliseconds kDefaultBackgroundMaxTimeout = std::chrono::minutes(10);
// Live background tasks per session: a dev server, a watcher, a long build at
// once; past it the model is told, not queued.
inline constexpr int kDefaultMaxBackgroundTasks = 4;
// The persisted result's cap (the model's view); live streaming is uncapped.
// Sized for a context window, not a disk: dense output (paths, hex, JSON)
// runs 1-2 bytes per token, so this is ~16-32k tokens. Past it the start and
// the end are kept and the whole output spills to a file the model can read.
inline constexpr std::int64_t kDefaultMaxOutputBytes = 64 * static_cast<std::int64_t>(common::kKiB);

// --- the shell wrapper ------------------------------------------------------------

// The fd the wrapper writes session state (cwd, env) to: a high, rarely used
// number. Any fd can collide with the command's own; correctness rests on
// validating the payload (ParseStatePayload), not on the number.
inline constexpr int kStateFd = 9;
// The state payload is `pwd -P` plus `env -0` -- kilobytes. Past this a
// command pumping the state fd is discarded wholesale (never a partial
// environment applied).
inline constexpr std::size_t kMaxStatePayloadBytes = common::kMiB;
// After the shell is reaped, output pipes still open mean a detached
// grandchild holds them; quiet this long ends the run with a teaching note
// instead of holding the call to its deadline. One default, no knob.
inline constexpr std::chrono::milliseconds kIdleAfterReap = std::chrono::seconds(2);

// The display payload's output tail: what a client renders, well inside
// tools.max_display_bytes even for the long runs where the exit badge matters.
inline constexpr std::size_t kDisplayOutputTailBytes = 16 * common::kKiB;

// --- command analysis ---------------------------------------------------------------

// Literal nesting inside one command name (concatenation, quoting): real
// commands are a few levels; past this the name is "computed", not named.
inline constexpr int kMaxLiteralDepth = 64;
// Wrapper programs peeled per stage (`sudo env X=1 nice rm`): never deep in
// practice; the bound only stops a pathological chain.
inline constexpr int kMaxWrapperPeelDepth = 6;

}  // namespace minimal::tools_bash
