#pragma once

// client policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

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

// --- paging -----------------------------------------------------------------------

// Sessions per ListSessions page (pickers, `-l`, subagent scans).
inline constexpr std::int32_t kSessionListPageSize = 200;
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
// Trailing stdout lines per running tool in the tools panel, when ui.json
// sets none.
inline constexpr int kDefaultToolsPanelStdoutLines = 100;

// --- display widths and previews ------------------------------------------------------

// A tool call's input preview on its row, and a live tool summary.
inline constexpr std::size_t kToolInputSummaryBytes = 120;
inline constexpr std::size_t kLiveToolSummaryBytes = 200;
// An attachment longer than this starts collapsed.
inline constexpr std::size_t kAttachmentCollapseThresholdChars = 2000;
// Picker columns: a session/preset name, a key spec in the keymap help, and
// the message-count column ("12345 msgs").
inline constexpr std::size_t kNameColumnCap = 40;
inline constexpr std::size_t kKeyColumnCap = 18;
inline constexpr std::size_t kMessageCountColumnWidth = 9;
// `minimal -l`'s name column.
inline constexpr std::size_t kSessionListNameColumnChars = 48;
// An unnamed subagent is labelled by its session id's prefix: the ULID's
// timestamp part plus a little randomness, enough to tell siblings apart.
inline constexpr std::size_t kUnnamedSessionIdPrefixChars = 12;

// Transient notices: cleared after this many ticks of UI activity, and never
// before they have been visible this long (a streaming background tab is
// activity too). Picked by feel.
inline constexpr int kNoticeTicksDefault = 20;
inline constexpr std::chrono::milliseconds kNoticeMinVisible = std::chrono::seconds(4);

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

}  // namespace minimal::client
