#pragma once

// client policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "minimal/client/sdk/policy.hpp"
#include "minimal/common/units.hpp"

namespace minimal::client {

// --- server connection ----------------------------------------------------------

// Probe for an already-running server before spawning one: the common case,
// answered in well under this.
inline constexpr std::chrono::milliseconds kServerProbeTimeout = std::chrono::seconds(2);
// A spawned server must become reachable within this: a cold start (plugins,
// database, provider discovery kicked off) takes seconds; this reports a
// server that will never come up without leaving the user staring for long.
inline constexpr std::chrono::milliseconds kServerStartupTimeout = std::chrono::seconds(25);
// Readiness poll while waiting for a spawned server.
inline constexpr std::chrono::milliseconds kServerStartupPoll{250};
// The channel's connect wait once a server is known reachable.
inline constexpr std::chrono::seconds kChannelConnectTimeout{5};

// Unary RPC deadlines. Every call is bounded: a wedged (or still-spawning)
// server surfaces as DEADLINE_EXCEEDED, never a hang of the asking thread.
// Listings, lookups, deletes, one scrollback page:
inline constexpr std::chrono::seconds kUnaryDeadline{2};
// Session creation and plugin calls touch the database or plugin code:
inline constexpr std::chrono::seconds kCreateDeadline{5};
inline constexpr std::chrono::seconds kPluginCallDeadline{5};
// The keep-alive: a missed beat costs nothing, so it gives up quickly.
inline constexpr std::chrono::seconds kPingDeadline{1};
// A model-list refresh waits on every discover entry's catalog fetch.
inline constexpr std::chrono::seconds kListModelsRefreshDeadline{15};
// @-mention suggestions answer while the user types.
inline constexpr std::chrono::milliseconds kSuggestPathsDeadline{250};
// Detach waits this long for its writer to flush before cancelling: any
// healthy round trip, far short of hanging quit.
inline constexpr std::chrono::seconds kDetachJoinTimeout{2};

// Outbound events queued on one stream: a whole turn's control traffic
// several times over; past it the stream is stalled and Send() refuses.
inline constexpr std::size_t kStreamSendQueueCap = 256;
// RPCs queued for a background RPC worker; the stream closer queues a
// group's worth of Detach/Close jobs, which must never be refused.
inline constexpr std::size_t kRpcQueueCapacity = 64;
inline constexpr std::size_t kStreamCloserQueueCapacity = 1024;

// --- `minimal status-bar` ---------------------------------------------------------

// The width a sample render is measured against when stdout is not a
// terminal: a common terminal width, wide enough to show every segment of
// the bundled layout.
inline constexpr int kStatusBarSampleWidth = 120;

// --- paging -----------------------------------------------------------------------

// Messages per scrollback page, and the pages one refresh job may walk before
// yielding: a bounded job cannot wedge the worker behind a huge delta.
inline constexpr std::int32_t kScrollbackPageSize = 100;
inline constexpr int kMaxScrollbackPagesPerRefresh = 50;
// Failed-refresh backoff (from the second consecutive failure): doubling from
// the base to the ceiling, so a slow server is retried soon and a dead one
// is not hammered.
inline constexpr std::chrono::milliseconds kScrollbackRetryBase{250};
inline constexpr std::chrono::milliseconds kScrollbackRetryMax = std::chrono::seconds(5);
// Consecutive refresh failures before the tab shows a visible error notice.
inline constexpr int kScrollbackFailureNoticeAfter = 3;

// --- memory bounds (ui.json defaults) ----------------------------------------------

// Messages kept in a tab's scrollback window; older ones reload on scroll.
inline constexpr int kDefaultScrollbackWindowMaxMessages = 2000;
// One tool's live output kept per row (head and tail); the persisted result
// is the record.
inline constexpr int kDefaultToolOutputBufferBytes = 64 * static_cast<int>(common::kKiB);
// Below this, the head + marker + tail split of a tool's output has no room;
// a ui.json value under it is raised to it.
inline constexpr int kMinToolOutputBufferBytes = 4 * static_cast<int>(common::kKiB);
// Live text of the streaming turn: far past what a human reads mid-turn; the
// turn-end refresh re-fetches the persisted text.
inline constexpr std::size_t kLiveTextCapBytes = 2 * common::kMiB;
// Live segments of the streaming turn (newest kept; the status bar says so).
inline constexpr std::size_t kMaxLiveSegments = 100;

// --- display widths and previews ------------------------------------------------------

// A live tool's one-line summary (the tab activity, the tools panel).
inline constexpr std::size_t kLiveToolSummaryBytes = 200;
// An attachment longer than this starts collapsed.
inline constexpr std::size_t kAttachmentCollapseThresholdChars = 2000;
// `minimal -l`'s name column.
inline constexpr std::size_t kSessionListNameColumnChars = 48;


// Slot ids for compaction rows start here, far past any real message index,
// so the two id spaces never meet.
inline constexpr int kCompactionSlotBase = 1 << 20;

// --- loops --------------------------------------------------------------------------

// The UI's background tick (running-tool redraws, notice expiry): smooth
// enough for a spinner, cheap enough to run for a whole session.
inline constexpr std::chrono::milliseconds kUiTick{50};
// Headless: how long a Ctrl-C waits for the turn handle to exist (it races
// the turn's start), and the poll inside that wait.
inline constexpr std::chrono::milliseconds kHeadlessCancelWait = std::chrono::seconds(5);
inline constexpr std::chrono::milliseconds kHeadlessCancelPoll{5};

// --- chat view ---------------------------------------------------------------------

// Highlighted code blocks cached (MRU): a screenful of blocks and some
// scrollback.
inline constexpr std::size_t kHighlightCacheCap = 64;


// --- prompt editor -------------------------------------------------------------------

// @-mention suggestions: this many per query.
inline constexpr int kMentionSuggestionCount = 20;
// Prompt history: entries kept (far past what arrow-up recall is used for),
// and the persisted file's cap (a pasted log must not make every submit
// rewrite megabytes).
inline constexpr std::size_t kMaxPromptHistoryEntries = 500;
inline constexpr std::size_t kMaxPromptHistoryFileBytes = common::kMiB;

// --- layout -----------------------------------------------------------------------------

// Below this terminal height only the critical rows (editor, status bar) are
// kept, so a fixed-size row is never squeezed to nothing and overdrawn.
inline constexpr int kCriticalTerminalHeight = 8;
// The notice line's scrolling height.
inline constexpr int kNoticeMaxHeight = 6;

// --- plugins -------------------------------------------------------------------------

// Consecutive throws from one plugin surface (a status segment, an observer)
// before it is skipped for the rest of the session: a broken plugin must not
// log an error per repaint, and one transient failure is forgiven.
inline constexpr int kMaxPluginThrows = 3;

}  // namespace minimal::client
