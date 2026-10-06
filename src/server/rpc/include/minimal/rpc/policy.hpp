#pragma once

// rpc policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::rpc {

// --- event fan-out (EventBroadcaster) -------------------------------------------

// Bytes buffered for one live subscriber: a ceiling that catches a stalled
// client, not a tuning knob. Overflow drops oldest and latches a resync; the
// transcript, not the buffer, is the durable log. (The event-count bound is
// common::kDefaultEventQueueMaxSize.)
inline constexpr std::size_t kSubscriberMaxBytes = 64 * common::kMiB;
// The zero-subscriber backlog: nobody reads it, so it is pure resident memory
// per detached session, kept far below a live subscriber's.
inline constexpr std::size_t kBacklogMaxBytes = 8 * common::kMiB;

// --- read-only command pool (CommandExecutor) -------------------------------------

// Threads and queued tasks for commands run off the reader thread: a handful
// of concurrent /help-style commands; past the backlog a request is refused.
inline constexpr std::size_t kCommandExecutorThreads = 4;
inline constexpr std::size_t kCommandExecutorBacklog = 64;
// A read-only command that runs longer than this is abandoned (its thread
// keeps running, uncounted): far past any listing or report.
inline constexpr std::chrono::milliseconds kCommandDeadline = std::chrono::minutes(1);
// Abandoned command threads tolerated before the executor refuses work: a
// command that wedges every time must not accumulate threads forever.
inline constexpr std::size_t kMaxAbandonedCommands = 8;
// Same-key commands in flight at once.
inline constexpr std::size_t kMaxCommandsPerKey = 2;
// The deadline watchdog's tick: a fraction of the deadline, capped so a long
// deadline is still checked often, floored so a tiny one does not spin.
inline constexpr int kWatchdogTicksPerDeadline = 4;
inline constexpr std::chrono::milliseconds kMaxWatchdogTick{250};
inline constexpr std::chrono::milliseconds kMinWatchdogTick{10};

// --- server lifecycle ---------------------------------------------------------------

// In-flight RPCs get this long to finish at shutdown before they are
// cancelled: a turn mid-stream finishes or is cut, never left to wedge exit.
inline constexpr std::chrono::milliseconds kShutdownGrace = std::chrono::seconds(30);
// Housekeeping tick (idle exit, checkpoints): an idle deadline is honored
// within a quarter second.
inline constexpr std::chrono::milliseconds kHousekeepingTick{250};
// How often a plugin-subscription stream rechecks its client's cancellation
// while nothing is being sent.
inline constexpr std::chrono::milliseconds kSubscriptionCancelPoll{500};

// --- paging and answers ---------------------------------------------------------------

// Page size when settings.json leaves scrollback_page_size unset: a screenful
// or two of messages per round trip.
inline constexpr int kDefaultPageSize = 100;
// A plugin prompt's answer from the wire: answers are small structured
// choices; past this one reads as dismissed.
inline constexpr std::size_t kMaxPluginPromptAnswerBytes = 256 * common::kKiB;

}  // namespace minimal::rpc
