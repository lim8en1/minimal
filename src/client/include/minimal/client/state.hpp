#pragma once

#include <chrono>
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

        // Streaming assistant response (partial markdown building up)
        std::string streaming_text;          // empty when not streaming
        bool is_streaming_finalized = false;

        // Pending modals
        std::optional<PendingPermission> pending_permission;
        std::optional<PendingConfirmation> pending_confirmation;

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

    // --- Streaming response ---
    void AppendStreamingText(std::string_view text);
    void FinalizeStreamingText();

    // --- Turn boundaries (ToolExecution / TextDelta set Working/Thinking; these clear it) ---
    void OnTurnCompleted(const v1::TurnCompletedEvent& evt);
    void OnError(const std::string& error_message);

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

    // --- Pending interactions ---
    void SetPermissionRequest(std::unique_ptr<PendingPermission> req);
    void SetConfirmationRequest(std::unique_ptr<PendingConfirmation> req);
    void ClearPendingFor(std::string_view request_id);

    // A pending prompt was just answered. The server worker was BLOCKED on
    // this answer, so no further event is guaranteed to re-derive the tab
    // state: a command confirmation (/clear, /permission-mode) produces no
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

    // Records the optimistic echo of a turn the user just submitted (called
    // by SendChatTurn after the event was accepted onto the wire). Cleared by
    // the next page apply that carries a seq past the submit-time baseline --
    // the persisted user row, with the server's @-mention expansion, replaces
    // the echo -- or by an error/session end, which leave nothing to echo.
    void OnChatSubmitted(const std::string& text);

    // Resolves an outstanding async refresh that came back EMPTY or failed:
    // bumps refresh_seq without touching any content, so the frame that
    // requested it can tell "resolved, nothing changed" from "still
    // waiting" and is free to retry on the next mismatch. ApplyScrollbackPage
    // bumps refresh_seq itself on the success path.
    void NoteRefreshFailed();

private:
    void MaybeGoIdleLocked();   // mtx_ held by caller

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
    bool scrollback_trimmed_ = false;
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

    // Session metadata (see Snapshot).
    std::string model_;
    std::string agent_preset_;
    std::string permission_mode_;
    int64_t context_window_tokens_ = 0;

    // Streaming assistant text
    std::string streaming_accumulated_;
    bool is_streaming_ = false;
    uint64_t turn_seq_ = 0;    // increments when a NEW TURN's stream starts
    bool turn_ended_ = true;   // armed by OnTurnCompleted/OnError; disarmed by the next delta

    // Pending modals
    std::optional<PendingPermission> pending_permission_;
    std::optional<PendingConfirmation> pending_confirmation_;

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
    std::string session_id;
    std::shared_ptr<SessionState> state;      // never moves in the vector

    std::optional<std::string> parent_session_id;   // nullopt => root session
    int64_t root_ordinal = 0;         // when its ROOT tab was created (groups children)
    uint32_t spawn_seq = 0;           // global monotonic counter: stable sort key
};

class AppState {
public:
    explicit AppState(std::shared_ptr<SessionClient> client);

    struct TabSnapshot {
        std::string session_id;
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

    std::string OpenTab(const SessionClient::CreateOptions& opts);

    // Attach a session already known to the server (resumes its stream). Used when
    // the picker picks an existing session, or when a child subagent starts.
    //   parent_ordinal anchors grouping; -1 => treat as new root group.
    std::string AttachTab(const std::string& session_id, int64_t parent_ordinal = -1);

    // Resume a STORED session (the session picker's "open a past session"
    // action): re-issues CreateSession with resume_session_id so the server
    // revives the session from its transcript BEFORE the stream names it --
    // bare attach answers NOT_FOUND for a stored session, and the dead
    // stream used to swallow every prompt/command silently. A tab already
    // open (or registered by a racing call) is returned without
    // duplicating; a resume the server REFUSES still registers the tab --
    // in the Error state, with the refusal as its visible notice -- so a
    // failed pick is something the user can see and close. Always returns
    // `session_id` (the registered tab's id); focus-if-empty follows
    // OpenTab's rule, and the picker focuses explicitly afterward.
    std::string ResumeTab(const std::string& session_id);

    // Called by the reader thread for every ServerEvent whose payload is not bound to
    // a tab the client already owns (e.g. SubagentStarted on the PARENT's stream).
    void OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt);

    void CloseTab(const std::string& session_id);
    bool HasTab(const std::string& session_id) const;

    // --- Sending on a tab's stream (UI calls these; routes ClientEvents out) ---

    void SendChatTurn(const std::string& session_id, const std::string& text);

    // A `/name args` prompt from the prompt editor. Split client-side (the same
    // parser the headless flag uses) and sent as a CommandInvocation; the
    // CommandResult arrives back as the tab's transient notice.
    void SendCommand(const std::string& session_id, const std::string& line);

    // Ctrl-style interrupt of the tab's in-flight turn, if any.
    void SendCancel(const std::string& session_id);

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
    // ROLE_USER message and returns its seq, or nullopt if fewer than n
    // user turns are present in the loaded window.
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

    // --- Focus (keybindings cycle forward/back through tabs in render order) ---
    void CycleFocusForward();
    void CycleFocusBackward();
    void SetFocusedTab(const std::string& session_id);

    // --- Rendering ---
    // in render order. `include_tool_output`: see TakeLightSnapshot -- pass
    // true only when about to render the running-tools panel.
    std::vector<TabSnapshot> GetAllTabs(bool include_tool_output = false) const;

    // The focused tab's session id ("" when no tabs). The prompt editor and
    // status bar target it.
    std::string FocusedSessionId() const;

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

    // Memory bounds for every SessionState created AFTER this call
    // (OpenTab/AttachTab). 0 leaves that bound at its built-in default.
    // RunTui applies ui.json's values right after construction, before any
    // tab can open.
    void SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes);

    // DEEP snapshot of ONE tab -- the focused tab, for the frame. The pre-R
    // frame deep-copied EVERY tab (under each tab's lock) two to three times
    // per frame; with the light tab list (GetAllTabs -> TakeLightSnapshot)
    // and this, a frame copies the focused tab once.
    SessionState::Snapshot GetDetails(const std::string& session_id) const;

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
    // off the loop thread. For a call site that needs a synchronous
    // AppState method itself (OpenTab, whose CreateSession RPC carries a 5s
    // deadline) run off-thread, without widening that method's own contract
    // -- every AppState method's locking is already safe from any thread,
    // so this relocates WHERE it runs rather than changing what it does or
    // returns, which matters here because OpenTab's synchronous
    // session-id-on-return contract is load-bearing for the existing E2E
    // test suite (app_state_test.cpp) and for callers that legitimately
    // need it immediately (headless mode, tests).
    void RunOnRpcQueue(std::function<void()> job) { rpc_queue_.Submit(std::move(job)); }

private:
    struct TabEntry {
        TabInfo info;
        std::shared_ptr<SessionClient::Stream> stream;   // persistent bidi (+ threads)
    };

    // Routes a ServerEvent to its owned tab (by event.session_id()). Handles derived
    // state (state_, tokens, pending modals). Subagent-started events are deferred and
    // applied after the lock is released so we never open a stream while holding it.
    void DispatchToTab(const std::string& session_id, const v1::ServerEvent& event);

    mutable std::mutex mtx_;
    std::vector<TabEntry> tabs_;

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

    void FocusWithWrap(bool forward);       // shared logic for the two cycle methods
    TabEntry* FindTab(const std::string& session_id);  // unlocked, mutex held by caller
    std::shared_ptr<SessionClient::Stream> GetStream(const std::string& session_id);

    // Refuses VISIBLY when the tab's session has ended (wire SessionEnded /
    // stream close): the server drops everything for a terminated session --
    // usually silently -- so a send into it must not be queued. Returns true
    // (and surfaces the error on the tab) when sending must stop. `verb`
    // words the refusal ("send", "answer", "cancel").
    bool RefuseIfEnded(const std::string& session_id, const char* verb);
    // OWNING copy: a raw pointer here escaped the lock and could be destroyed
    // by CloseTab (which erases the TabEntry and drops the shared_ptr) while
    // the reader thread was still between the lookup and the call.
    std::shared_ptr<SessionState> GetState(const std::string& session_id) const;

    // Applies the resolved ui.json bounds (0 = built-in default) to a
    // freshly constructed SessionState -- identical in OpenTab and AttachTab.
    void ApplyUiBounds(SessionState& state) const;
    // Wires the stream's on-closed handler (transport-death vs. attach-refusal
    // vs. clean-close routing) for `session_id` -- identical in OpenTab and
    // AttachTab.
    void WireOnClosed(SessionClient::Stream& stream, const std::string& session_id);
};

}  // namespace minimal::client
