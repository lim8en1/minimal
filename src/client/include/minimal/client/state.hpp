#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/v1/common.pb.h"
#include "minimal/v1/session_service.pb.h"
#include "minimal/client/chat_model.hpp"
#include "minimal/client/rpc_queue.hpp"
#include "minimal/client/session_client.hpp"

namespace minimal::client::plugins {
class AttentionState;
class ChatSurface;
}  // namespace minimal::client::plugins

namespace minimal::client {

// ============================================================================
// Tool execution tracking (for the running-tools panel)
//   Server emits: ToolExecutionEvent{started=true} -> ToolOutputDeltaEvents ->
//   ToolExecutionEvent{started=false, is_error}, per tool_use_id. Tool dispatch
// is parallel (ARCHITECTURE.md "Tool dispatch is parallel", width 4 by
// default), so SEVERAL tools can be active at once and resolve in completion
// order, not gate order; every delta/finish event carries the tool_use_id
// matching its start — we key on that, never on position.
// ============================================================================

struct ToolExecutionState {
    std::string tool_use_id;
    std::string tool_name;
    std::optional<std::string> summary;      // human-readable from ToolExecutionEvent
    std::chrono::milliseconds started_at_unix_ms{-1};  // server-supplied timestamp

    // Accumulated chunks arrive here (lossy ring buffer). The final authoritative
    // record is the persisted ToolResultBlock, but this is what the panel shows.
    std::string buffered_output;

    bool started = false;      // true once we've seen a start event for this tool
    bool is_error = false;     // set by the finished ToolExecutionEvent (is_error)

    // True for a row opened by BackgroundTaskEvent{STARTED} rather than
    // ToolExecutionEvent{started:true} -- ARCHITECTURE.md "Background bash
    // rows": the task can outlive the ToolExecutionEvent{finished} that
    // acked the original (synchronous) call, possibly by several turns, so
    // OnTurnCompleted's per-turn tools_.clear() must leave these rows alone
    // (a live background row reopens under the same tool_use_id after its
    // ack, and closes only on ITS OWN terminal BackgroundTaskEvent).
    bool is_background = false;
};

// ============================================================================
// The current turn's live content, in ARRIVAL order (track C4).
//
// A turn's blocks are strictly SEQUENTIAL on the wire (a tool_use block's
// content_block_stop always precedes the next block's first delta, per the
// streaming protocol) -- text and tool activity never interleave WITHIN one
// block, but a turn as a whole is exactly the sequence "text, then a tool
// round, then more text, ..." that this records. Rendering the OLD single
// flattened buffer-plus-all-tool-rows-at-the-end (ChatView::Render, pre-C4)
// lost that order: text emitted before AND after a tool call fused into one
// block, with every tool row below both, however far apart the model
// actually put them. This is what lets the live view match the FINAL
// (persisted) row order instead of only converging to it at turn end.
// ============================================================================

struct LiveTurnSegment {
    enum class Kind { Text, Tool };
    Kind kind = Kind::Text;
    // Which turn this segment belongs to (SessionState::turn_seq_ at the time
    // the segment was created). The newest-page clear retires segments of the
    // turn the persisted page covers while leaving a newer turn's live text
    // alone -- without the stamp there is no way to tell them apart.
    uint64_t turn_seq = 0;
    // Kind::Text: this segment's OWN accumulated text (one content block's
    // worth) -- NOT the whole turn's text, unlike Snapshot::streaming_text.
    std::string text;
    // True once `text` hit SessionState::kStreamingTextCapBytes: further
    // deltas for THIS segment are dropped (the truncation marker inside
    // `text` was appended once). The transcript itself is authoritative --
    // the turn-end refresh re-fetches the full persisted text -- so the
    // truncation is presentation-only.
    bool text_truncated = false;
    // Kind::Tool: which tool -- cross-referenced against Snapshot::tools by
    // id at render time; this struct carries no tool state of its own so
    // ToolExecutionState stays the single source of truth for it.
    std::string tool_use_id;
};

// ============================================================================
// Pending interactions awaiting user input (permission/confirmation modals)
// ============================================================================

struct PendingPermission {
    std::string request_id;
    std::string tool_name;
    v1::ToolCategory category;   // READ_ONLY / FILE_WRITE / EXECUTE (UI tint)
    std::string summary;         // one-line description shown in the prompt
    bool complete = false;       // server offered full disclosure (false = lower bound)

    // Grant offers to present as session-grant buttons ("/usr/bin/make *"). Blanket
    // permission for the whole binary, any args. Worded exactly that way.
    std::vector<v1::GrantOffer> grants;
};

struct PendingConfirmation {
    std::string request_id;
    std::string kind;    // "tool_permission" | "command_confirmation"
    std::string title;
    std::string detail;
    std::vector<std::string> options;
};

// A model-initiated ask (the server tool-ask plugin): 1-4 structured
// questions rendered as one modal. The request is kept verbatim -- the modal
// needs every question, option, description, and flag.
struct PendingAsk {
    std::string request_id;
    v1::AskUserRequest request;
};

// ============================================================================
// Session state kinds (derived from wire events, per ARCHITECTURE.md)
// ============================================================================

enum class SessionStateKind {
    Idle,        // ready for input
    Thinking,   // assistant text streaming in (TextDelta received)
    Working,    // one or more tool executions pending completion
    Waiting,    // permission/confirmation blocking the turn - attention glyph !
    Done,       // TurnCompletedEvent received without an error
    Error      // ErrorEvent received on the stream
};

// ============================================================================
// Per-session state (one SessionState per UI tab). Thread-safe; updated by the
// reader callback and read by a snapshot every frame.
// ============================================================================

class SessionState {
public:
    explicit SessionState(std::string session_id, std::string name = "");

    // Memory bounds (track R2). Defaults; AppState applies ui.json's values
    // right after construction, before any event can arrive.
    static constexpr int kDefaultScrollbackWindowMaxMessages = 2000;
    static constexpr int kDefaultToolOutputBufferBytes = 64 * 1024;

    // Streaming caps (see the members below). 2 MiB of live text is far past
    // what a human reads mid-turn; the transcript is authoritative -- the
    // turn-end refresh re-fetches the full persisted text.
    static constexpr std::size_t kStreamingTextCapBytes = 2 * 1024 * 1024;
    // Live-segment count cap: the NEWEST segments are kept; on first
    // overflow a single "[...truncated]" marker segment is left at the
    // oldest position so the cut is visible, never silent.
    static constexpr std::size_t kMaxLiveSegments = 100;
    // Consecutive scrollback-refresh failures after which the tab surfaces a
    // visible error notice (see NoteRefreshFailed). Below this the existing
    // frame-pass backoff (scrollback_refresh.hpp) handles the quiet retry;
    // the notice exists so a stuck transcript is not a silent one.
    static constexpr int kScrollbackFailureNoticeAfter = 3;
    void SetBounds(int scrollback_max_messages, int tool_output_buffer_bytes);

    // Renderable copy taken under a short lock held minimally (no blocking I/O).
    struct Snapshot {
        std::string session_id;
        std::string display_name;
        SessionStateKind state = SessionStateKind::Idle;

        // Scrollback window (keyset-paged). Most recent N messages, descending.
        // SHARED, copy-on-apply: the page only changes when a fetch applies,
        // so snapshots taken between applies (one per wire event, on the
        // maintenance pass) share one buffer instead of deep-copying up to
        // scrollback_max_messages_ messages per event -- per-delta copies
        // during a busy turn were the render freeze. Never null.
        std::shared_ptr<const std::vector<v1::StoredMessage>> scrollback_page;
        bool has_earlier_messages = false;   // paging back will yield more
        int64_t last_loaded_seq = -1;        // cursor for the next "load older"

        // The turn the user JUST submitted, echoed optimistically: the
        // persisted row (with the server-side @-mention expansion) only
        // reaches a page via a fetch, and mid-turn nothing fetched -- so a
        // submitted message did not render until the turn ENDED, after the
        // assistant's streaming reply. Rendered by the chat view ahead of the
        // streaming block; cleared once a page apply proves the real row
        // landed (any seq newer than the submit-time baseline), or on error.
        std::string pending_user_text;

        // Streaming assistant response (partial markdown building up). The
        // FLAT concatenation of every live_segments Text entry (blocks
        // joined by a blank line, see AppendStreamingText) -- kept for
        // consumers that only care "is anything streaming" or need the
        // total text as one string (scrollback_refresh.cpp's refresh-latch
        // check, frame_renderer.cpp's fresh-session check), and for
        // TabMaintenance to diff against for the per-turn refresh-eligible
        // check. The ORDERED view a chat surface actually renders from is
        // live_segments below (track C4) -- streaming_text alone lost
        // ordering relative to tool calls that happened between its blocks.
        std::string streaming_text;          // empty when not streaming
        // Live thinking content (ThinkingDeltaEvent deltas, flat like
        // streaming_text). Accumulated here so a thinking turn loses NOTHING
        // on the live path -- before this field existed, thinking deltas were
        // dropped client-side and materialized only in the turn-end refetch,
        // a visible content jump. Cleared at exactly the same sites as
        // streaming_text (ClearStreaming / ClearStreamingIfCurrentTurn /
        // OnResyncRequired / OnSessionRewound).
        std::string streaming_thinking_text;  // empty when not streaming
        bool is_streaming_finalized = false;
        // This turn's live content in arrival order (LiveTurnSegment above).
        // Empty exactly when streaming_text is (same clear sites:
        // ClearStreaming, OnResyncRequired).
        std::vector<LiveTurnSegment> live_segments;

        // Pending modals
        std::optional<PendingPermission> pending_permission;
        std::optional<PendingConfirmation> pending_confirmation;
        std::optional<PendingAsk> pending_ask;

        // Active tools in gate order (the order the model asked for them);
        // several may be RUNNING at once (parallel dispatch, width 4 by
        // default) and finish in completion order, not this order.
        std::vector<ToolExecutionState> tools;

        // Status-bar occupancy from the LAST turn only (NOT cumulative — see ARCH).
        int64_t input_tokens = 0;
        int64_t output_tokens = 0;
        // Also last-turn-only, also from Usage (TurnCompletedEvent), also
        // discarded until now: cache hit/miss for the turn that just ran.
        // Cumulative totals -- across turns, in $ -- are NOT tracked here;
        // that's the bundled cost-tracker server plugin's job (it already
        // does this, see plugins/server/cost-tracker), consumed by a paired
        // client plugin rather than duplicated in core state.
        int64_t cache_read_tokens = 0;
        int64_t cache_write_tokens = 0;

        // Bumped whenever the persisted transcript changed in a way the chat
        // view should re-fetch (new scrollback page applied, turn completed).
        // The view compares its cached value to decide a refresh; no timer.
        uint64_t transcript_version = 0;

        // True when older-page loads evicted pages from the window's NEWEST
        // end to hold the cap: the top of the window is no longer the live
        // newest message, so the view renders a boundary marker instead of a
        // silent gap. Cleared by the newest-slice refresh at turn end.
        bool scrollback_trimmed = false;

        // True when an older-page apply could not be stitched contiguously
        // onto the window (a server-side hole between the fetched rows and
        // what we already held -- e.g. rows deleted or lost across a rewind):
        // there are seq values missing INSIDE the window, which no boundary
        // marker at either end can account for. The view can render a gap
        // marker for it the way it does scrollback_trimmed. Cleared by the
        // newest-slice refresh (the same reset scrollback_trimmed gets).
        bool scrollback_gap = false;

        // Increments once per streaming turn (new stream start). The frame's
        // maintain pass keys stream recreation on this instead of the old
        // buffer-size heuristic, which could splice two rapid turns.
        uint64_t turn_seq = 0;

        // The latest /-command outcome, for the transient notice line, or a
        // client-side error (a refused send, a dead stream, a failed resume).
        // NEVER a chat row (EXTENSIBILITY §4.4) — that is a type-level
        // property of CommandNotice, not a rendering choice made here. The
        // display/view_hint fields carry the E5 CommandDisplay refinement.
        std::optional<CommandNotice> latest_notice;

        // Monotonic per-session id for latest_notice: the VIEW side-effects
        // (open the picker, show a plugin modal) are performed ONCE per notice
        // by the frame's maintain pass, keyed on this — never from Render(),
        // which runs every frame and must stay side-effect-free for view
        // routing.
        uint64_t notice_seq = 0;

        // The user asked to interrupt the in-flight turn and the turn has not
        // ended yet (TurnCompleted / Error / session end all clear it). The
        // status bar renders the focused session as "cancelling…" while this
        // stands, so the press is never silent.
        bool cancel_requested = false;

        // --- Session metadata (the E5 emission, one per attach + on change) ---
        // The wire source for the status bar's {model} {agent}
        // {permission_mode} segments and the `max` half of {tokens}. Empty/0 =
        // unknown; the segment renders nothing, per the unknown-segment rule.
        std::string model;
        std::string agent_preset;
        std::string permission_mode;
        int64_t context_window_tokens = 0;

        // True when the LAST turn ended (TurnCompleted / Error) and no new
        // turn has begun. The frame's maintain pass fetches the newest
        // scrollback slice only on a turn boundary: streaming_text stays
        // non-empty until the refresh itself swaps it for persisted rows,
        // so "text empty" cannot tell "turn over" from "mid-turn" -- this
        // flag can.
        bool turn_ended = false;

        // Bumped by every RESOLVED async scrollback-refresh attempt (success
        // OR failure) -- see AppState::RequestScrollback / rpc_queue.hpp. A
        // successful apply also bumps transcript_version, but a FAILED fetch
        // does not touch it at all -- so a frame that only watched
        // transcript_version to learn "my outstanding request resolved"
        // could never distinguish "still waiting" from "it failed, stop
        // waiting and let the next mismatch retry", and would latch on a
        // failure forever (the bug this counter exists to fix: T2's "the
        // next trigger retries" comment used to be false).
        uint64_t refresh_seq = 0;

        // The outcome of the refresh attempt that bumped refresh_seq: a
        // resolved SUCCESSFUL fetch means what is rendered can be marked
        // current even when the page applied no new rows (an empty page
        // moves no version -- retrying it every frame was a 20 Hz GetMessages
        // loop). A resolved FAILED attempt leaves the version mismatch in
        // place so the next frame retries.
        bool refresh_succeeded = false;
    };

    Snapshot TakeSnapshot();

    // LIGHT variant for the per-frame tab list (track R: the focused-tab pump
    // redesign): everything the tab bar and the CROSS-SESSION tools panel
    // render, WITHOUT the heavy copies -- no scrollback page, no streaming
    // text, no pending-modal data. GetAllTabs() calls this per tab per frame;
    // the deep TakeSnapshot() runs for the FOCUSED tab only.
    //
    // `include_tool_output`: each ToolExecutionState::buffered_output can hold
    // up to tool_output_buffer_bytes_ (64 KiB default) of live stdout, and
    // GetAllTabs() is called on nearly every event (the tab bar needs the
    // list every render; the maintenance pass needs it every event just to
    // find which tab is focused) -- copying every tool's full buffer on every
    // one of those calls was pure waste when the caller only needed
    // name/state/summary, which is the common case. false (the default)
    // leaves buffered_output empty; only a caller that is actually about to
    // render the running-tools panel's live tail passes true.
    Snapshot TakeLightSnapshot(bool include_tool_output = false);

    // --- Session lifecycle ---
    void OnSessionCreated(std::string name);
    void OnSessionMetadata(const v1::SessionMetadataEvent& metadata);

    // Sessionless-tab support. A tab's SessionState is constructed before its
    // session exists (session_id "" and no metadata), so EnsureSession tells
    // the state its id once the create resolves, and the /agent //model
    // staging paths seed the status-bar fields directly (the wire metadata
    // event only arrives once a session exists).
    void SetSessionId(std::string session_id);
    void StageDisplayModel(std::string model_ref);
    void StageDisplayAgentPreset(std::string preset_name);

    // --- Streaming response ---
    void AppendStreamingText(std::string_view text);
    // Thinking deltas, the mirror of AppendStreamingText for the model's
    // thinking blocks: same cap-and-truncate budget, same turn arming, but a
    // dedicated accumulation (live_segments_ model only Text and Tool
    // segments, matching how the persisted turn records are rendered) so the
    // view layer can present thinking separately from text.
    void AppendStreamingThinking(std::string_view text);
    void FinalizeStreamingText();

    // --- Turn boundaries (ToolExecution / TextDelta set Working/Thinking; these clear it) ---
    void OnTurnCompleted(const v1::TurnCompletedEvent& evt);
    void OnError(const std::string& error_message);

    // Mid-turn usage (message_start/message_delta). Keeps {tokens} live
    // during a turn; OnTurnCompleted still overwrites with the final tally.
    void OnUsage(const v1::UsageEvent& evt);

    // /-command outcome. Stored as a transient notice for the notice line;
    // never enters the chat rows.
    void OnCommandResult(const v1::CommandResult& evt);

    // The user asked to interrupt the in-flight turn. A no-op unless the tab
    // is actually mid-turn (Thinking / Working / Waiting -- Waiting counts:
    // the server's Cancel answers a parked permission/confirmation prompt
    // with a denial, so a wedged prompt is interruptible too).
    void RequestCancel();

    // Drops the accumulated streaming text. The chat view calls this after a
    // post-turn scrollback refresh, when the streamed message has become a
    // persisted row -- otherwise the same content would render twice.
    void ClearStreaming();

    // Turn-aware ClearStreaming (AppState::RequestScrollback's newest apply).
    // The persisted page covers the turn that was live at SUBMIT time
    // (`turn_seq` captured then); a DIFFERENT turn that started since owns the
    // live text now, and clearing it wiped the new turn's opening deltas until
    // it ended -- a visible transcript gap. A matching turn_seq clears exactly
    // as ClearStreaming does (the common case: turn ended, no new turn).
    void ClearStreamingIfCurrentTurn(uint64_t turn_seq);

    // The live turn's identity (Snapshot::turn_seq) without a full snapshot --
    // what RequestScrollback captures at submit time for the check above.
    uint64_t CurrentTurnSeq() const;

    // The session's stream ended (server closed it / session terminated) --
    // the real "Done" (✓) signal, distinct from a turn merely completing.
    void OnSessionEnded();

    // True once OnSessionEnded ran: the session is terminated server-side and
    // the server will silently drop everything sent to it. Senders check this
    // to refuse visibly instead of queueing into a void (the stream itself
    // often stays open after a session_ended -- the server's read loop does
    // not close on termination).
    bool IsEnded() const;

    // The server reported a HOLE in this stream's event delivery
    // (ResyncRequiredEvent): drop-oldest overflow on this subscriber's queue,
    // or a late attach that missed events another subscriber took. Events are
    // ephemeral; the durable record is the transcript, so the response is to
    // discard the possibly-incomplete live material (streaming segments) and
    // let the frame pump refetch the newest scrollback.
    void OnResyncRequired(const std::string& reason);

    // The server cut or restored the transcript tail (SessionRewoundEvent):
    // rewind trims the window to the ACTIVE transcript (rows with seq <=
    // active_last_seq); redo (truncated_to_seq == -1) restores rows the client
    // has never seen, so the whole window is cleared for the refresh to
    // refetch. Either way streaming/pending-turn state goes with the rows it
    // belonged to. Not a turn terminator: no TurnCompletedEvent accompanies it.
    void OnSessionRewound(int64_t truncated_to_seq, int64_t active_last_seq);

    // --- Pending interactions ---
    void SetPermissionRequest(std::unique_ptr<PendingPermission> req);
    void SetConfirmationRequest(std::unique_ptr<PendingConfirmation> req);
    void SetAskRequest(std::unique_ptr<PendingAsk> req);
    // Clears any pending modal matching `request_id`. Returns TRUE when
    // something was actually cleared -- callers that must re-derive the tab
    // state afterwards (the worker was parked on that prompt) use the return
    // value so a no-op clear (the request was never ours, or already
    // answered) does not fire the re-derive spuriously.
    bool ClearPendingFor(std::string_view request_id);

    // A pending prompt was just answered. The server worker was BLOCKED on
    // this answer, so no further event is guaranteed to re-derive the tab
    // state: a command confirmation (/clear) produces no
    // tool events or message stops afterwards, and without this call the tab
    // stays stranded in Waiting with the '!' glyph and nothing pending.
    void OnPendingResolved();

    // --- Tool execution (stream order, keyed by tool_use_id) ---
    void OnToolStarted(const v1::ToolExecutionEvent& evt);
    void OnToolOutputDelta(const std::string& tool_use_id, const std::string& chunk);
    void OnToolFinished(const std::string& tool_use_id, const v1::ToolExecutionEvent& final_evt);

    // ARCHITECTURE.md "Background bash rows": a BackgroundTaskEvent rides the
    // same tool_use_id as the ToolExecutionEvent that acked the original
    // (synchronous) call, so STARTED opens or REOPENS a row -- a row closed
    // by its ack legitimately reopens when the detached task later reports a
    // terminal state, possibly many turns afterward. OnToolOutputDelta
    // already keys by tool_use_id + started, so background output deltas are
    // routed correctly with no change there.
    void OnBackgroundTask(const v1::BackgroundTaskEvent& evt);

    // --- Scrollback paging (keyset by seq) ---
    void ApplyScrollbackPage(const v1::GetMessagesResponse& resp);
    bool CanLoadMore() const;

    // In-flight latch for "load older" fetches (AppState::RequestScrollback,
    // newest=false). Two presses inside one RPC round trip fetch the SAME
    // before_seq page, and the second apply used to fall through to the
    // newer/assign branch -- replacing the window with the older page, newest
    // rows vanishing. TryBeginOlderFetch claims the slot (false = one is
    // outstanding, the caller no-ops); EndOlderFetch resolves it on BOTH apply
    // paths, success and failure.
    bool TryBeginOlderFetch();
    void EndOlderFetch();

    // Records the optimistic echo of a turn the user just submitted (called
    // by SendChatTurn after the event was accepted onto the wire). Cleared by
    // the next page apply that carries a seq past the submit-time baseline --
    // the persisted user row, with the server's @-mention expansion, replaces
    // the echo -- or by an error/session end, which leave nothing to echo.
    void OnChatSubmitted(const std::string& text);

    // Resolves an outstanding async scrollback refresh that came back EMPTY
    // or failed: bumps refresh_seq without touching any content, so the frame
    // that requested it can tell "resolved, nothing changed" from "still
    // waiting" and is free to retry on the next mismatch. ApplyScrollbackPage
    // bumps refresh_seq itself on the success path.
    //
    // `reason` (optional) is the RPC's error message: counted across
    // consecutive failures, the kScrollbackFailureNoticeAfter-th failure in a
    // row surfaces a visible error notice on the tab's notice line -- the
    // server refuses a transcript page with INTERNAL on an undecodable row,
    // and before this the client retried forever with no user-visible
    // symptom. Retrying itself stays the frame pass's job (its backoff is
    // untouched); the notice only ends the silence.
    void NoteRefreshFailed(const std::string& reason = {});

private:
    void MaybeGoIdleLocked();   // mtx_ held by caller

    // Shared by TakeSnapshot/TakeLightSnapshot(include_tool_output=false):
    // `buffered_output` can hold up to tool_output_buffer_bytes_ (64 KiB
    // default) per tool, and ChatView::RenderLiveToolRow -- the deep snapshot's
    // only tool consumer -- reads name/started/is_error/summary, never the
    // buffer (the running-tools panel is what renders it, off the LIGHT
    // snapshot with include_tool_output=true). TakeSnapshot used to copy the
    // buffer anyway on every focused-tab deep snapshot; this is the one place
    // that decision is made now. mtx_ held by caller.
    std::vector<ToolExecutionState> CopyToolsLocked(bool include_output) const;

    mutable std::mutex mtx_;

    std::string session_id_;
    std::string display_name_;   // tab/picker label; refreshed from server metadata
    SessionStateKind state_ = SessionStateKind::Idle;

    // Scrollback window (descending by seq: front = newest). BOUNDED: loading
    // older pages beyond scrollback_max_messages_ evicts from the newest end
    // and sets scrollback_trimmed_ -- an unbounded append let a long PageUp
    // materialize a 1M-token transcript in client memory.
    std::vector<v1::StoredMessage> scrollback_page_;
    // The shared, immutable view of scrollback_page_ handed out by
    // TakeSnapshot. Rebuilt (one copy) at the end of ApplyScrollbackPage --
    // the only mutation site -- so every snapshot between applies shares it.
    std::shared_ptr<const std::vector<v1::StoredMessage>> scrollback_page_shared_ =
        std::make_shared<const std::vector<v1::StoredMessage>>();
    int64_t last_loaded_seq_ = -1;      // oldest seq loaded so far (descending)
    bool has_earlier_ = false;
    uint64_t transcript_version_ = 0;
    uint64_t refresh_seq_ = 0;   // see Snapshot::refresh_seq
    bool refresh_succeeded_ = false;   // outcome of the attempt that bumped refresh_seq_
    // Consecutive RESOLVED-FAILED scrollback fetches; reset by every
    // successful apply. Drives the one-shot error notice in NoteRefreshFailed
    // (retry pacing itself lives in scrollback_refresh.hpp's per-tab backoff).
    int scrollback_failures_ = 0;
    bool scrollback_trimmed_ = false;
    bool scrollback_gap_ = false;   // see Snapshot::scrollback_gap
    int scrollback_max_messages_ = kDefaultScrollbackWindowMaxMessages;
    int tool_output_buffer_bytes_ = kDefaultToolOutputBufferBytes;

    // The optimistic user-turn echo (see Snapshot::pending_user_text). The
    // baseline is the page's NEWEST seq at submit time (front of the
    // descending window): any later apply whose newest seq is past it proves
    // the persisted row (appended FIRST by the server, before any turn
    // output) is in the page and the echo can go.
    std::string pending_user_text_;
    int64_t pending_user_baseline_seq_ = -1;

    // Latest /-command outcome (transient notice line; never a chat row).
    // notice_seq_ advances on every OnCommandResult so consumers can tell a
    // fresh notice from a re-snapshot of the same one.
    std::optional<CommandNotice> latest_notice_;
    uint64_t notice_seq_ = 0;

    // Set by RequestCancel, cleared when the turn actually ends.
    bool cancel_requested_ = false;

    // The "load older" in-flight latch (see TryBeginOlderFetch). Never read
    // outside mtx_.
    bool older_fetch_pending_ = false;

    // Session metadata (see Snapshot).
    std::string model_;
    std::string agent_preset_;
    std::string permission_mode_;
    int64_t context_window_tokens_ = 0;

    // Streaming assistant text
    // CAPPED (track R2): a runaway turn must not grow client memory (or the
    // per-frame deep snapshot, which copies both members) without limit.
    // streaming_accumulated_ stops at kStreamingTextCapBytes -- further
    // deltas are dropped once a truncation marker has been appended; the
    // count of live_segments_ stops at kMaxLiveSegments (newest kept, a
    // single "[...truncated]" marker left at the oldest position). The
    // transcript itself is authoritative: the post-turn refresh re-fetches
    // the full persisted text (scrollback_refresh.hpp).
    std::string streaming_accumulated_;
    // Live thinking accumulation (see Snapshot::streaming_thinking_text):
    // same cap-and-truncate budget as the flat text buffer, plus the turn
    // stamp the turn-aware clear sites need to retire it only when the
    // persisted page actually covers its turn.
    std::string thinking_accumulated_;
    bool thinking_truncated_ = false;       // thinking_accumulated_ hit its cap
    uint64_t thinking_turn_seq_ = 0;        // the turn the accumulation belongs to
    bool streaming_truncated_ = false;      // streaming_accumulated_ hit its cap
    bool live_segments_truncated_ = false;  // the marker segment is in place
    bool is_streaming_ = false;
    uint64_t turn_seq_ = 0;    // increments when a NEW TURN's stream starts
    bool turn_ended_ = true;   // armed by OnTurnCompleted/OnError; disarmed by the next delta
    // This turn's live content in arrival order (track C4) -- see
    // Snapshot::live_segments. Same clear sites as streaming_accumulated_.
    std::vector<LiveTurnSegment> live_segments_;

    // Pending modals
    std::optional<PendingPermission> pending_permission_;
    std::optional<PendingConfirmation> pending_confirmation_;
    std::optional<PendingAsk> pending_ask_;

    // Tool executions (parallel dispatch; keyed by tool_use_id, not position)
    std::vector<ToolExecutionState> tools_;

    int64_t input_tokens_ = 0;   // last turn occupancy
    int64_t output_tokens_ = 0;
    int64_t cache_read_tokens_ = 0;
    int64_t cache_write_tokens_ = 0;
};

// ============================================================================
// App-level state: ordered tabs (roots + their subagent children), focus, derived UI.
// ============================================================================

struct TabInfo {
    // CLIENT-GENERATED tab identity: the primary key for every UI operation
    // (focus, close, the tab_ui map, pending_close, keybinding targets). A tab
    // exists before its session does (OpenTab mints nothing -- the session is
    // created when the first message is sent), so session_id cannot serve as
    // the key: every sessionless tab would collide on "".
    std::string tab_id;
    std::string session_id;                   // empty until the session exists
    std::shared_ptr<SessionState> state;      // never moves in the vector

    std::optional<std::string> parent_session_id;   // nullopt => root session
    int64_t root_ordinal = 0;         // when its ROOT tab was created (groups children)
    uint32_t spawn_seq = 0;           // global monotonic counter: stable sort key
};

// TEMPORARY DEBUG globals removed -- see git history for the deadlock diagnosis.
class AppState {
public:
    explicit AppState(std::shared_ptr<SessionClient> client);

    struct TabSnapshot {
        std::string tab_id;            // the UI's tab handle (focus/close/map keys)
        std::string session_id;        // empty while the tab has no session
        bool has_session = false;      // what autocomplete / the /help overlay filter on
        std::string display_name;      // for tab bar / picker
        SessionStateKind state = SessionStateKind::Idle;
        bool is_subagent = false;
        int64_t root_ordinal = 0;
        uint32_t spawn_seq = 0;

        // UI-derived
        char attention_glyph = '\0';     // '!' when Waiting or plugin-raised, else nothing
        bool is_focused = false;

        // The tab's tool rows (bounded buffers, R2) -- what the cross-session
        // running-tools panel renders. Filled by the LIGHT snapshot; before it
        // existed the panel forced a full deep Snapshot of EVERY tab per frame.
        std::vector<ToolExecutionState> tools;

        // The focused tab's interrupt request still awaiting its turn end.
        bool cancel_requested = false;
    };

    // --- Tab management (all thread-safe) ---
    //
    // TAB vs SESSION. A tab is a client-side UI container holding a NULLABLE
    // session reference. OpenTab creates ONLY the tab -- no RPC, no server-side
    // row -- and the session comes into existence when the first message is
    // sent (SubmitChatTurn -> EnsureSession), or never for a tab the user
    // abandons. This is the fix for the 0-message junk sessions a startup or
    // picker create used to leave behind: nothing is persisted until there is
    // something to say. Tabs are keyed by a client-generated tab_id (every
    // sessionless tab would collide on an empty session_id); the session-keyed
    // senders below remain the wire layer and are only reached through a tab
    // that HAS a session.

    // Opens a SESSIONLESS tab carrying `opts` as its pending create-options
    // (applied verbatim when the session is eventually created -- /model and
    // /agent choices a user makes before the first message are staged here).
    // Returns the new tab's tab_id. Never blocks, never touches the network.
    std::string OpenTab(const SessionClient::CreateOptions& opts);

    // Attach a session already known to the server as a tab (resumes its
    // stream). Used when the picker picks a live session, or when a child
    // subagent starts.
    //   parent_ordinal anchors grouping; -1 => treat as new root group.
    // Returns the new tab's tab_id ("" on refusal/duplicate).
    std::string AttachTab(const std::string& session_id, int64_t parent_ordinal = -1);

    // Resume a STORED session (the session picker's "open a past session"
    // action): re-issues CreateSession with resume_session_id so the server
    // revives the session from its transcript BEFORE the stream names it --
    // bare attach answers NOT_FOUND for a stored session, and the dead
    // stream used to swallow every prompt/command silently. A tab already
    // open (or registered by a racing call) is returned without
    // duplicating; a resume the server REFUSES still registers the tab --
    // in the Error state, with the refusal as its visible notice -- so a
    // failed pick is something the user can see and close. Always returns the
    // registered tab's tab_id; focus-if-empty follows OpenTab's rule, and the
    // picker focuses explicitly afterward.
    std::string ResumeTab(const std::string& session_id);

    // Called by the reader thread for every ServerEvent whose payload is not bound to
    // a tab the client already owns (e.g. SubagentStarted on the PARENT's stream).
    void OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt);

    // Both keyed by TAB id now (the session_id overload would be ambiguous for
    // sessionless tabs, whose session_id is "").
    void CloseTab(const std::string& tab_id);
    bool HasTab(const std::string& tab_id) const;

    // --- Sending on a tab's stream (UI calls these; routes ClientEvents out) ---

    // THE SEND ENTRIES for the prompt editor. Tab-keyed, and the only paths
    // that can CREATE a session: on a tab whose session does not exist yet the
    // action is queued and EnsureSession runs on the RPC worker (single-flight
    // per tab), so a burst of submissions before the create resolves still
    // lands on ONE session. On a sessioned tab both delegate inline, exactly
    // like the session-keyed senders always did (fire-and-forget enqueue; no
    // loop-thread blocking either way).
    void SubmitChatTurn(const std::string& tab_id, const std::string& text);
    void SubmitCommand(const std::string& tab_id, const std::string& line);

    // The wire layer under the Submit* entries. Session-keyed: only legal on a
    // tab that HAS a session (the Submit* wrappers route around them
    // otherwise; a direct call with an unknown/empty id surfaces a visible
    // refusal on the tab).
    void SendChatTurn(const std::string& session_id, const std::string& text);

    // A `/name args` prompt from the prompt editor. Split client-side (the same
    // parser the headless flag uses) and sent as a CommandInvocation; the
    // CommandResult arrives back as the tab's transient notice.
    void SendCommand(const std::string& session_id, const std::string& line);

    // Ctrl-style interrupt of the tab's in-flight turn, if any.
    void SendCancel(const std::string& session_id);

    // permission.cycle (shift+tab): a client-native wire event, like
    // SendCancel/SendRewind -- no command, no confirmation modal (the
    // keybinding IS the confirmation). The server queues the change for its
    // worker and announces it back via SessionMetadataEvent, which is what
    // the {permission_mode} status segment refreshes from.
    void SendSetRunMode(const std::string& session_id, v1::PermissionRunMode mode);

    // Rewind is a client-native wire event (a oneof sibling of cancel/attach),
    // not a routed command -- sent directly like SendCancel, never through
    // SendCommand. Exactly one of target_seq/redo is meaningful: pass a seq
    // to rewind to it (see RewindRequest in session_service.proto for the
    // "discard from target_seq onward" semantics), or std::nullopt with
    // redo=true to restore the most recently discarded tail.
    void SendRewind(const std::string& session_id, std::optional<int64_t> target_seq, bool redo);

    // The client has no other way to let a user target a rewind -- raw
    // transcript seq numbers are never surfaced in the UI. Scans the tab's
    // current scrollback window backward for the n-th (1 = most recent)
    // TEXT-BEARING ROLE_USER message and returns its seq, or nullopt if
    // fewer than n user turns are present in the loaded window. Text-bearing
    // (any non-empty TextBlock) is the SERVER's own IsRewindTarget predicate
    // (agent/session.cpp): counting bare User rows here as well would let
    // /rewind N target an injected subagent completion -- a position the
    // user cannot re-enter as a prompt -- and be refused server-side.
    std::optional<int64_t> FindRewindTargetSeq(const std::string& session_id, int n);

    // Asynchronously re-fetches the scrollback window for a tab and applies
    // it (T2: this used to run the GetMessages RPC synchronously on the
    // FTXUI loop thread, including from inside a Renderer lambda -- a merely
    // slow server stalled the whole UI for up to the RPC's 2s deadline on
    // every frame that changed transcript_version).
    //   newest=true: replace the window with the freshest page (post-turn).
    //   newest=false: page OLDER via the keyset cursor (scroll-up "load more").
    // Returns immediately; enqueues the fetch on the async RPC worker
    // (rpc_queue.hpp). The result publishes into SessionState under its own
    // mutex and bumps refresh_seq (Snapshot) so the caller can tell a
    // resolved attempt from one still in flight, and BumpUiVersion() so the
    // pump's next tick redraws from it.
    void RequestScrollback(const std::string& session_id, bool newest);

    // Asynchronously resolves @-mention path suggestions (T2: this used to
    // run SuggestPaths synchronously on the loop thread, inside the prompt
    // editor's own Renderer lambda, on the keystroke after every '@' -- a
    // merely slow server stalled typing for up to the RPC's 250 ms deadline).
    // `on_result` runs on the RPC worker thread with the resolved list
    // (empty on any failure -- the same "closes the list" behaviour the
    // synchronous call had) and must not block; it is the caller's job to
    // recognize a stale answer (the `suggest` lambda in app.cpp keys its
    // in-flight tracking on session id + prefix).
    void RequestSuggestPaths(const std::string& session_id, const std::string& prefix,
                             int32_t limit, std::function<void(std::vector<std::string>)> on_result);

    // --- User answers to pending prompts (UI calls these; routes ClientEvents back) ---

    void AnswerPermission(const std::string& session_id, const std::string& request_id,
                         v1::PermissionDecision decision, std::vector<std::string> granted_keys);

    void AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                            const std::string& chosen_option);
    void AnswerAsk(const std::string& session_id, const v1::AskUserResponse& response);

    // --- Focus (keybindings cycle forward/back through tabs in render order) ---
    void CycleFocusForward();
    void CycleFocusBackward();
    void SetFocusedTab(const std::string& tab_id);

    // --- Sessionless-tab staging (/agent, /model before the session exists) ---

    // Stages a model/agent choice onto a tab WITHOUT a session (validated by
    // the caller against the session-free ListModels/ListAgentPresets RPCs).
    // The choice rides the tab's pending create-options when the session is
    // eventually created, and seeds the tab's display metadata immediately so
    // the status bar reflects it. Returns false (a no-op) when the tab already
    // has a session -- callers send the real /model //agent command there.
    bool StageModel(const std::string& tab_id, const std::string& model_ref);
    bool StageAgentPreset(const std::string& tab_id, const std::string& preset_name);

    // Runs `action` on the RPC worker once the tab's session exists (creating
    // it if needed; single-flight per tab, queued actions share one create).
    // On a sessioned tab, runs inline. `action` receives the session id and
    // must not block.
    void OnTabEnsureSession(const std::string& tab_id,
                            std::function<void(const std::string& session_id)> action);

    // Runs `line` as a SESSIONLESS command (the server's RunSessionlessCommand
    // RPC, executed on the async RPC worker) and surfaces the result on the
    // tab's notice line -- the same display/view_hint routing an on-stream
    // command result gets. Only for tabs without a session; a sessioned tab
    // uses the ordinary SubmitCommand path.
    void RunSessionlessCommand(const std::string& tab_id, const std::string& line);

    // Surfaces `message` on the tab's notice line (a kStatus command-result
    // shape, so the frame renders it like any other outcome). The staging
    // paths (/agent //model on a sessionless tab) report through this.
    void TabNotice(const std::string& tab_id, const std::string& message);

    // --- Rendering ---
    // in render order. `include_tool_output`: see TakeLightSnapshot -- pass
    // true only when about to render the running-tools panel.
    std::vector<TabSnapshot> GetAllTabs(bool include_tool_output = false) const;

    // The focused tab's session id ("" when no tabs, or when the focused tab
    // has no session yet). The session-keyed senders target it.
    std::string FocusedSessionId() const;

    // The focused tab's TAB id ("" when no tabs) -- the UI's handle for it,
    // valid whether or not a session exists yet.
    std::string FocusedTabId() const;

    // Closes (terminates) every tab. Used by ctrl+w's explicit close, not
    // quit -- see DetachAllTabs for that (S5.4: a session now outlives the
    // client that opened it, so quitting the app is no longer the same
    // action as terminating every open session).
    void CloseAllTabs();

    // Detaches every tab: the TUI's quit path. Every session this client was
    // attached to keeps running server-side; a turn in flight completes
    // unattended, and the session is resumable/re-attachable afterward.
    void DetachAllTabs();

    // --- E5 extension seams (host-owned; both optional) ----------------------

    // The shared plugin-attention block (docs/EXTENSIBILITY.md 4.2): a plugin
    // raise WINS over core derivation until cleared; the glyph stays '!'.
    // Called once before any reader thread exists; not thread-of-use safe, the
    // block itself is.
    void SetPluginAttention(plugins::AttentionState* attention) {
        plugin_attention_ = attention;
    }

    // The exclusive chat surface, when a plugin claimed the slot. DispatchToTab
    // pushes EVERY ServerEvent to it (reader thread; OnEvent must not block)
    // before core's own handling -- a surface owns its presentation, core keeps
    // its data plane regardless.
    void SetChatSurface(plugins::ChatSurface* surface) { chat_surface_ = surface; }

    // The exclusive chat surface, unless a plugin threw out of it (catch +
    // disable): null means the core chat view renders the tab body.
    plugins::ChatSurface* EffectiveChatSurface() const {
        return chat_surface_broken_.load(std::memory_order_acquire) ? nullptr : chat_surface_;
    }

    // Disables a surface that threw mid-render (loop thread). `reason` may be
    // null for a non-standard exception.
    void DisableChatSurface(const char* reason);

    // Wakes the 20 Hz pump for state published OUTSIDE any AppState mutator
    // -- PickerController's async fetch completions, e.g. (ARCHITECTURE.md
    // review: picker RPCs used to run synchronously on the loop thread, up to
    // 15 s for a model-discovery refresh). Every ordinary AppState mutation
    // already bumps this same counter at its own entry point; a caller with
    // no AppState state of its own to mutate has no other way to get the same
    // wake. Safe from any thread, like every other AppState method.
    void NotifyExternalUpdate() { BumpUiVersion(); }

    // Memory bounds for every SessionState created AFTER this call
    // (OpenTab/AttachTab). 0 leaves that bound at its built-in default.
    // RunTui applies ui.json's values right after construction, before any
    // tab can open.
    void SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes);

    // DEEP snapshot of ONE tab -- the focused tab, for the frame. The pre-R
    // frame deep-copied EVERY tab (under each tab's lock) two to three times
    // per frame; with the light tab list (GetAllTabs -> TakeLightSnapshot)
    // and this, a frame copies the focused tab once. Keyed by TAB id (the
    // focused handle may be a sessionless tab).
    SessionState::Snapshot GetDetails(const std::string& tab_id) const;

    // Monotonic UI revision, bumped by every mutating entry point. The 20 Hz
    // pump posts a redraw ONLY when this moved (plus a low-rate floor for the
    // editor's debounce gate) -- an idle TUI previously repainted the full
    // frame 20 times per second no matter what.
    uint64_t UiVersion() const {
        return ui_version_.load(std::memory_order_acquire);
    }

    // Joins the async RPC worker (rpc_queue.hpp; RequestScrollback and
    // friends). Call once, after the render pump stops posting frames and
    // BEFORE CloseAllTabs() -- a queued job's completion callback touches
    // SessionState/AppState the same way a reader thread's dispatch does,
    // and CloseAllTabs() tears down the very tabs a straggling job would
    // publish into.
    void StopRpcQueue() { rpc_queue_.Stop(); }

    // Escape hatch: runs an arbitrary job on the same async RPC worker,
    // off the loop thread. For a call site that needs a blocking AppState
    // method itself (AttachTab/ResumeTab, whose CreateSession/Attach RPCs
    // carry 5s deadlines; EnsureSession likewise) run off-thread, without
    // widening that method's own contract
    // -- every AppState method's locking is already safe from any thread,
    // so this relocates WHERE it runs rather than changing what it does or
    // returns, which matters here because the blocking tab openers'
    // synchronous contract is load-bearing for the existing E2E
    // test suite (app_state_test.cpp) and for callers that legitimately
    // need it immediately (headless mode, tests).
    //
    // `must_run` defaults to TRUE: everything routed through this hatch
    // today resolves a latch or a user action (EnsureSession's queued
    // sends, the picker's DeleteSession) and must never be dropped by the
    // queue's eviction (rpc_queue.hpp). A genuinely droppable keystroke-
    // class job (RequestSuggestPaths) submits directly with must_run=false.
    void RunOnRpcQueue(std::function<void()> job, bool must_run = true) {
        rpc_queue_.Submit(std::move(job), must_run);
    }

private:
    struct TabEntry {
        TabInfo info;
        std::shared_ptr<SessionClient::Stream> stream;   // null while the tab is sessionless

        // The CreateOptions a sessionless tab carries, applied verbatim by
        // EnsureSession. Meaningless once `stream` exists (the options went
        // out with the CreateSession that made it).
        SessionClient::CreateOptions pending_create;

        // Single-flight create for a sessionless tab (the per-tab form of the
        // queue-and-create pattern that used to live in RunTui's
        // create_session_and_run). `ensuring` is only ever set on the RPC
        // worker (the sole EnsureSession runner); `pending` is pushed by any
        // thread under mtx_ and drained by the worker after the create
        // resolves. A failed create drops the pending actions -- their senders
        // see the tab's visible error notice, the same outcome a synchronous
        // failure always had.
        bool ensuring = false;
        std::vector<std::function<void(const std::string& session_id)>> pending;
    };

    // Routes a ServerEvent to its owned tab (by event.session_id()). Handles derived
    // state (state_, tokens, pending modals). Subagent-started events are deferred and
    // applied after the lock is released so we never open a stream while holding it.
    void DispatchToTab(const std::string& session_id, const v1::ServerEvent& event);

    mutable std::mutex mtx_;
    std::vector<TabEntry> tabs_;
    uint64_t next_tab_seq_ = 0;   // tab_id minting ("t1", "t2", ...)

    int64_t next_root_ordinal_ = 0;
    uint32_t next_spawn_seq_ = 0;
    std::string focused_id_;     // active tab ("" => none)

    // ui.json bounds (0 = built-in default), applied to every new SessionState.
    int scrollback_max_messages_ = 0;
    int tool_output_buffer_bytes_ = 0;

    std::atomic<uint64_t> ui_version_{0};
    void BumpUiVersion() { ui_version_.fetch_add(1, std::memory_order_acq_rel); }

    // T2: the async unary RPC worker backing RequestScrollback (and, over
    // time, the other calls app.cpp used to make on the loop thread). One
    // queue for the whole app, not per tab -- a single worker gives FIFO
    // ordering for free, so e.g. a queued delete can never race ahead of a
    // list queued after it.
    RpcQueue rpc_queue_;
    std::vector<std::pair<std::string, v1::SubagentStartedEvent>> pending_subagents_;   // deferred
    // Fast-path skip for DispatchToTab's drain step (see .cpp): true only
    // while pending_subagents_ actually holds something. Every call to
    // DispatchToTab -- one per wire event, on every tab's own reader thread
    // -- used to take mtx_ a SECOND time just to swap out an
    // almost-always-empty vector; subagent spawns are rare relative to
    // stream/tool events. Release-store paired with an acquire-load makes
    // the flag alone sufficient to skip the lock: a thread that observes
    // `false` here is guaranteed (release-acquire) to also see an empty
    // pending_subagents_ if it looked, without needing the mutex to say so.
    std::atomic<bool> has_pending_subagents_{false};

    std::shared_ptr<SessionClient> client_;

    // E5 seams (see the setters above).
    plugins::AttentionState* plugin_attention_ = nullptr;
    plugins::ChatSurface* chat_surface_ = nullptr;
    // A plugin surface that THREW is disabled for the process (catch +
    // disable): the core chat view takes over, the process survives. Third
    // thrown events downgrade to a logged error, not a dead tab. Atomic: every
    // reader thread writes it via DispatchToTab (see .cpp), the loop thread
    // writes it via DisableChatSurface, and both read it back through
    // EffectiveChatSurface -- a plain bool here was an unsynchronized
    // read/write race across threads (TSan-visible).
    std::atomic<bool> chat_surface_broken_{false};

    // Teardown latch (fix for "subagent attached concurrently with quit is
    // never detached"): set as the FIRST act of CloseAllTabs/DetachAllTabs,
    // BEFORE tabs_ is drained. OnSubagentSpawned checks it and refuses to
    // attach, so a SubagentStartedEvent landing on a reader thread during
    // quit can neither open a stream that no teardown will ever detach, nor
    // touch tabs_/SetFocusedTab mid-destruction.
    std::atomic<bool> shutting_down_{false};

    void FocusWithWrap(bool forward);       // shared logic for the two cycle methods
    // Unlocked, mutex held by caller. Tab-keyed (the UI's primary axis).
    TabEntry* FindTab(const std::string& tab_id);
    // Session-keyed lookups: wire-event routing (events carry session_id),
    // the senders, and subagent parenting -- all only ever target tabs that
    // have a session.
    TabEntry* FindTabBySession(const std::string& session_id);
    std::shared_ptr<SessionClient::Stream> GetStream(const std::string& session_id);

    // Refuses VISIBLY when the tab's session has ended (wire SessionEnded /
    // stream close): the server drops everything for a terminated session --
    // usually silently -- so a send into it must not be queued. Returns true
    // (and surfaces the error on the tab) when sending must stop. `verb`
    // words the refusal ("send", "answer", "cancel").
    // RPC-worker only: creates the session for a sessionless tab (Stream::Open
    // with the staged create-options), registers the stream, then drains the
    // entry's pending actions. Returns the session id ("" on refusal, with the
    // error surfaced visibly on the tab).
    std::string EnsureSession(const std::string& tab_id);

    // Kicks EnsureSession onto the RPC worker (callers hold no lock).
    void RunEnsure(const std::string& tab_id);

    bool RefuseIfEnded(const std::string& session_id, const char* verb);
    // OWNING copy: a raw pointer here escaped the lock and could be destroyed
    // by CloseTab (which erases the TabEntry and drops the shared_ptr) while
    // the reader thread was still between the lookup and the call.
    std::shared_ptr<SessionState> GetState(const std::string& session_id) const;
    // TAB-keyed variant (a sessionless tab has no session id to look up by).
    std::shared_ptr<SessionState> GetStateByTab(const std::string& tab_id) const;

    // Applies the resolved ui.json bounds (0 = built-in default) to a
    // freshly constructed SessionState -- identical in OpenTab and AttachTab.
    void ApplyUiBounds(SessionState& state) const;
    // Wires the stream's on-closed handler (transport-death vs. attach-refusal
    // vs. clean-close routing) for `session_id` -- identical in OpenTab and
    // AttachTab.
    void WireOnClosed(SessionClient::Stream& stream, const std::string& session_id);
};

}  // namespace minimal::client
