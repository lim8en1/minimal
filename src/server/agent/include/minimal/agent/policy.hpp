#pragma once

// agent policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::agent {

// --- estimates ---------------------------------------------------------------

// The bytes-per-token ratio behind every token ESTIMATE in the tree (the
// compactor's fallback, plugins sizing a cap in tokens). A convenience
// heuristic, not a tokenizer: real counts vary by model and content, and
// anything with a backend-reported count uses that instead.
inline constexpr std::int64_t kEstimatedBytesPerToken = 4;
// The ratio for SIZING a cap that must hold in tokens (a tool result's share
// of the window), deliberately pessimistic: dense output -- paths, hex, JSON,
// escaped control bytes -- tokenizes at 1-2 bytes per token, so a cap sized
// at 4 would let through two to four times the tokens it was meant to allow.
inline constexpr std::int64_t kConservativeBytesPerToken = 2;

// The floor under a window-derived tool-result cap: a tiny or misreported
// window must still leave room for an error message and a spill note.
inline constexpr std::int64_t kMinToolResultCapBytes = 4096;

// JsonSizeEstimate's size for a number (or binary): sized without printing
// it. A fixed guess is within the noise of kEstimatedBytesPerToken.
inline constexpr std::int64_t kEstimatedJsonNumberBytes = 8;

// --- context tail ordering (ContextContributor) -------------------------------

// Sort keys, ascending. Core's signal-channel instructions sit ahead of the
// plugin default; the gaps leave room on both sides for a plugin that needs
// to land before or after either.
inline constexpr int kDefaultContextPriority = 100;
inline constexpr int kSignalContextPriority = 50;

// --- bounded queues: ceilings against a runaway producer, not tuning knobs ----

// The session worker's queue (turns, commands, rewinds). One turn can run for
// minutes; a client streaming turns faster than that is refused past this,
// long after any human-paced backlog.
inline constexpr std::size_t kMaxQueuedWorkItems = 32;

// Messages staged for an idle parent session (subagent results, task
// completions). Each producer is itself rate-limited (max_concurrent_subagents,
// max_background_tasks), so this is far above any legitimate backlog.
inline constexpr std::size_t kMaxPendingInjections = 64;

// --- waits -------------------------------------------------------------------

// Retry backoff (exponential, equal jitter): the first wait and the ceiling.
// Half a second rides out a momentary blip; eight seconds keeps a rate-limited
// turn moving without hammering the backend. A Retry-After hint overrides.
inline constexpr std::chrono::milliseconds kRetryBaseDelay{500};
inline constexpr std::chrono::milliseconds kRetryMaxDelay{8000};

// Shared budget for activating a preset's tool-source members (MCP connect +
// tools/list) at session start. A local server answers in well under it; a
// dead one costs this much once instead of stalling the session.
inline constexpr std::chrono::seconds kToolSourceStartupBudget{5};

// The detached-session reaper's tick: a fraction of the attach guard keeps
// every deadline within one tick of its target without a timer per session;
// the floor keeps a tiny guard (tests, direct construction) from spinning.
inline constexpr int kReapTicksPerAttachGuard = 4;
inline constexpr std::chrono::milliseconds kMinReapInterval{250};

// --- tool results and output budget ------------------------------------------

// FitOutputCap: the request's max_tokens leaves the window this margin for
// the estimate being off -- a percent of the window, but never less than the
// minimum -- and never drops below the floor (a reply needs some room even
// when the estimate says the window is full; the backend has the last word).
inline constexpr std::int64_t kOutputCapMarginPercent = 2;
inline constexpr std::int64_t kOutputCapMinMarginTokens = 4096;
inline constexpr std::int64_t kOutputCapFloorTokens = 1024;

// Overflow recovery elides the largest tool result to a stub of this size; a
// result already this small is left alone (eliding it frees nothing).
inline constexpr std::int64_t kElisionTargetBytes = 500;
inline constexpr std::int64_t kElisionMinBytes = 1000;

// A compaction whose history is still at least this percent of its old size
// "freed little" and says so (the summary is about as large as what it
// replaced, or the kept tail is most of the history).
inline constexpr std::int64_t kCompactionFreedLittlePercent = 90;

// --- @-mentions ---------------------------------------------------------------

// A mention is a convenience window, not a substitute for the read tool: past
// these the block says it was truncated and the model reads more itself.
inline constexpr int kMentionMaxLines = 1000;
inline constexpr int kMentionMaxLineLength = 2000;
// Directory mentions: levels below the mentioned directory, and entries.
inline constexpr int kMentionTreeMaxDepth = 3;
inline constexpr std::size_t kMentionTreeMaxEntries = 500;
// Per message: without these, N mentions read N windows into one prompt.
// Past either, the mention stays verbatim for the model to read itself.
inline constexpr int kMaxMentionsPerMessage = 32;
inline constexpr std::size_t kMaxMentionBytesPerMessage = 8 * common::kMiB;

// Path suggestions returned when the client asks without a limit: more than
// a picker shows, few enough to walk cheaply.
inline constexpr int kDefaultMentionSuggestLimit = 50;

// --- display -----------------------------------------------------------------

// A session's default name (from its first turn): a tab label, not a summary.
inline constexpr std::size_t kDefaultSessionNameMaxChars = 60;

// How much of a tool's request a permission prompt quotes (a subagent's task,
// an ask question): enough to judge the call on one line.
inline constexpr std::size_t kPermissionPreviewChars = 100;

}  // namespace minimal::agent
