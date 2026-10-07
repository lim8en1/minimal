#pragma once

// client policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

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

// --- chat view ---------------------------------------------------------------------

// Live thinking text kept on screen while it streams: settled lines, and the
// trailing partial line's bytes ever re-wrapped. Presentation only; the
// transcript is authoritative.
inline constexpr std::size_t kMaxLiveThinkingLines = 300;
inline constexpr std::size_t kMaxLiveThinkingTailBytes = 64 * common::kKiB;
// Mouse wheel: events closer than the gate are dropped (about one step per
// two frames at a ~60 Hz repaint), and each accepted tick moves a few lines --
// small enough to read, large enough that a trackpad flick travels.
inline constexpr std::chrono::milliseconds kWheelGate{30};
inline constexpr int kWheelLinesPerTick = 3;
// Keyboard scroll step before the first layout pass has measured the viewport.
inline constexpr int kFallbackViewportLines = 20;
// Long row content renders in chunks of at most this many source lines, so a
// streamed delta re-renders one small chunk, not the whole row.
inline constexpr std::size_t kMaxAnchorLines = 20;
// An expanded attachment renders at most this much: an accidentally expanded
// huge file must not wedge the renderer.
inline constexpr std::size_t kAttachmentExpandedRenderCapChars = 200'000;
// A tool result's body shown on its row (the tail window): orientation, not
// an archive -- the persisted result is the record.
inline constexpr std::size_t kToolResultBodyPreviewBytes = 4000;

// Markdown of a streaming message is reparsed whole (md4c has no incremental
// API), at most this often; past the threshold the interval grows with the
// buffer up to the ceiling, so a long reply's tail still updates without the
// reparse eating every frame.
inline constexpr std::chrono::milliseconds kMarkdownReparseDebounce{120};
inline constexpr std::chrono::milliseconds kMaxMarkdownReparseDebounce{500};
inline constexpr std::size_t kMarkdownReparseScaleThresholdBytes = 8 * common::kKiB;
// Past the threshold the interval grows by a millisecond per this many bytes.
inline constexpr std::size_t kMarkdownReparseBytesPerExtraMs = 64;
// Highlighted code blocks cached (MRU): a screenful of blocks and some
// scrollback.
inline constexpr std::size_t kHighlightCacheCap = 64;

// A running tool's spinner advances one frame per interval: visibly alive,
// calm enough not to distract.
inline constexpr std::chrono::milliseconds kSpinnerFrameInterval{120};

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
// Height caps for scrolling regions: a picker's menu, a notice.
inline constexpr int kMenuMaxHeight = 20;
inline constexpr int kNoticeMaxHeight = 6;
// Modals: the default width (the body's hints get 78 columns inside a
// 2-column margin) and the confirm modal's narrower one; rows reserved around
// a modal body (borders and the pinned hint line, plus slack for the tab bar
// and editor it overlays); a body always gets at least the minimum.
inline constexpr int kModalMaxWidth = 82;
inline constexpr int kConfirmModalMaxWidth = 72;
inline constexpr int kModalFrameRows = 3;
inline constexpr int kModalOverlaySlackRows = 3;
inline constexpr int kMinModalBodyRows = 2;
// A modal's pinned rows (ModalFrame's `pinned`: a selected row's details, the
// last outcome) take at most this many rows.
inline constexpr int kModalPinnedMaxRows = 8;
// Status bar / tools panel / tab bar columns.
inline constexpr std::size_t kStatusSessionColumnCap = 24;
inline constexpr std::size_t kToolsPanelSessionColumn = 16;
inline constexpr int kTabOverflowMarkerWidth = 5;

// Context occupancy that tints the token segment: attention as compaction
// nears, error when the window is all but full.
inline constexpr std::int64_t kContextAttentionPercent = 80;
inline constexpr std::int64_t kContextErrorPercent = 95;

// Where a status segment ranks when the bar overflows (lowest gives way
// first) unless it declares its own priority.
inline constexpr int kPluginStatusSegmentPriority = 30;

// The schema form (/config): a field's label and value columns -- room for a
// dotted key and a typical value; longer ones are clipped, the full value is
// one keypress away in the editor.
inline constexpr std::size_t kSchemaFormLabelWidth = 28;
inline constexpr std::size_t kSchemaFormValueWidth = 30;

// --- plugins -------------------------------------------------------------------------

// A plugin view's data feed (checklist, cost): after its push subscription
// fails it marks the cache stale and polls every poll interval for the
// fallback period before subscribing again; consecutive failures double the
// period up to the ceiling, so a server without the plugin is not hammered.
inline constexpr std::chrono::milliseconds kPluginFeedPollInterval = std::chrono::seconds(3);
inline constexpr std::chrono::milliseconds kPluginFeedFallbackPeriod = std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kPluginFeedMaxFallbackPeriod = std::chrono::minutes(5);

// Consecutive throws from one plugin surface (a status segment, an observer)
// before it is skipped for the rest of the session: a broken plugin must not
// log an error per repaint, and one transient failure is forgiven.
inline constexpr int kMaxPluginThrows = 3;

}  // namespace minimal::client
