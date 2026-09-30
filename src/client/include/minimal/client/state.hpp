#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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
    // The model's call args (ToolExecutionEvent.input), for SummarizeToolInput
    // to build a one-line row summary from -- `summary` above is currently
    // always empty on the wire (rendering from tool_name + input is the
    // client's job; see agent_loop.hpp), so this is what the live row actually
    // has to work with.
    nlohmann::json input = nlohmann::json::object();
    std::chrono::milliseconds started_at_unix_ms{-1};  // server-supplied timestamp
    // Set by OnToolFinished from the finish event's duration_ms; -1 while the
    // tool is still running (elapsed is computed from started_at_unix_ms
    // instead -- see RenderLiveToolRow/RenderToolsPanel).
    std::chrono::milliseconds duration{-1};

    // Accumulated chunks arrive here (lossy ring buffer). The final authoritative
    // record is the persisted ToolResultBlock, but this is what the panel shows.
    //
    // SESSIONSTATE-INTERNAL working buffer: appended in place by
    // OnToolOutputDelta (O(delta)), never copied out to a snapshot. The
    // published view snapshots carry is `buffered_output` below -- rebuilt
    // lazily (copy-on-publish) only when this changed since the last publish.
    std::string buffered_output_working;

    // The published live-stdout view: WHAT SNAPSHOTS CARRY. Null unless the
    // snapshot was taken with include_tool_output (only the running-tools
    // panel renders it). Shared, immutable -- CopyToolsLocked(true) hands
    // every consumer the SAME pointer until the buffer next changes, so a
    // rebuild of the panel (or any other include-output fetch) no longer
    // deep-copies up to tool_output_buffer_bytes_ (64 KiB default) per tool
    // per fetch; it pays one refcount bump.
    // mutable: publishing (rebuilding this pointer from the working buffer)
    // happens inside CopyToolsLocked -- const, called from TakeLightSnapshot
    // -- the same cache-write-on-read rule as output_dirty below.
    mutable std::shared_ptr<const std::string> buffered_output;
    // Copy-on-publish stamp: set by every buffered_output_working mutation,
    // cleared when the pointer above is (re)published. mutable: publishing
    // happens inside CopyToolsLocked (const, called from TakeLightSnapshot).
    mutable bool output_dirty = true;

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
    enum class Kind { Text, Tool, Thinking };
    Kind kind = Kind::Text;
    // Which turn this segment belongs to (SessionState::turn_seq_ at the time
    // the segment was created). The newest-page clear retires segments of the
    // turn the persisted page covers while leaving a newer turn's live text
    // alone -- without the stamp there is no way to tell them apart.
    uint64_t turn_seq = 0;
    // Monotonic per-session identity, assigned at every push site. Stream
    // slots and render caches key on THIS, never on the vector index:
    // segments are REMOVED mid-turn (RetireCoveredLiveSegments' page apply,
    // TrimLiveSegmentsLocked's count cap, ClearStreamingIfCurrentTurn) and
    // the survivors shift down without any turn_seq bump -- an index-keyed
    // slot fed by seen-watermark appends spliced one segment's tail onto
    // another segment's buffered view, corrupted for the rest of the turn
    // (the reported "random unrelated text" in long thinking-heavy turns).
    // Ids are never reused, and removals are permanent within a turn, so
    // "id absent from the snapshot" is a sound slot-GC trigger.
    uint64_t segment_id = 0;
    // WORKING accumulation -- SessionState-internal only: appended by the
    // delta paths, prefix-matched by the retirement logic. Never copied to a
    // snapshot (it is the whole turn's O(text) churn -- see TakeSnapshot);
    // TakeSnapshot publishes `text_view` below instead.
    std::string text;
    // Copy-on-publish view, the same pattern as ToolExecutionState's
    // buffered_output (state.hpp): WHAT SNAPSHOTS CARRY. Republished -- one
    // copy of THIS segment's accumulated text -- only when the segment
    // changed since the last deep snapshot, so a turn's settled segments
    // share their view across every wire event that changes nothing but the
    // newest one, and the flat per-event copies are gone. Null for Tool
    // markers (which carry no text) and for empty segments.
    // mutable: publishing happens inside TakeSnapshot's segment copy, exactly
    // like the tool-output cache inside CopyToolsLocked.
    mutable std::shared_ptr<const std::string> text_view;
    mutable bool text_dirty = true;
    // True once `text` hit SessionState::kStreamingTextCapBytes: further
    // deltas for THIS segment are dropped (the truncation marker inside
    // `text` was appended once). The transcript itself is authoritative --
    // the turn-end refresh re-fetches the full persisted text -- so the
    // truncation is presentation-only.
    bool text_truncated = false;
    // Kind::Tool: which tool -- cross-referenced against Snapshot::tools by
    // id at render time, so ToolExecutionState stays the single source of
    // truth while the row lives in SessionState::tools_.
    std::string tool_use_id;
    // Kind::Tool: the row's final state, captured when OnTurnCompleted sweeps
    // it out of tools_ (output buffers not carried). The marker outlives that
    // sweep until the post-turn refresh retires it; without this copy it had
    // nothing to render and the turn's tool calls vanished whenever that
    // refresh was late. Null while the row is still in tools_. Shared and
    // immutable, so snapshots copy a pointer.
    std::shared_ptr<const ToolExecutionState> final_tool;

    // Snapshot-facing read of this segment's text: the published view when
    // one is carried, empty when not (Tool markers, empty segments). Callers
    // must treat the reference as valid only for the snapshot's lifetime.
    const std::string& Text() const {
        static const std::string kEmpty;
        return text_view ? *text_view : kEmpty;
    }
};

// ============================================================================
// Pending interactions awaiting user input (permission/confirmation modals)
// ============================================================================

// One optimistically echoed, not-yet-persisted user turn (see Snapshot::
// pending_user_messages). The server assigns the turn id when it accepts the
// submission (TurnQueuedEvent); 0 means the ack has not arrived yet.
struct PendingUserMessage {
    std::uint64_t turn_id = 0;
    std::string text;
    // The window's NEWEST seq at submit time: the persisted row provably
    // landed once a page carries a text-bearing user row past this -- paired
    // with entries in FIFO order, one row per entry.
    int64_t baseline_seq = -1;
};

struct PendingPermission {
    std::string request_id;
    std::string tool_name;
    v1::ToolCategory category;   // READ_ONLY / FILE_WRITE / EXECUTE (UI tint)
    std::string summary;         // one-line description shown in the prompt
    bool complete = false;       // server offered full disclosure (false = lower bound)
    std::string reason;          // the rule that made this call ask (display only)

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

// A plugin prompt (PluginPromptRequest): a server plugin's blocking question
// whose shape only the client plugin registered for `kind` understands (the
// tool-ask/ask-view pair's "ask" is the bundled one). Core parses the payload
// JSON and nothing more.
struct PendingPluginPrompt {
    std::string request_id;
    std::string kind;
    nlohmann::json payload;   // null when payload_json did not parse
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
    // Live-segment count cap: the NEWEST segments are kept; on overflow the
    // live_view_truncated_ latch stands and the status bar's {activity}
    // segment carries an "earlier rows hidden" suffix while it does -- the
    // cut is visible in chrome, never as a synthetic row in the stream.
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
        // Context compactions known for this session (from scrollback pages
        // and live CompactionEvents), ascending by at_seq. Shared,
        // copy-on-change like scrollback_page; never null. The chat view
        // renders each after the message at its at_seq.
        std::shared_ptr<const std::vector<v1::CompactionEvent>> compactions;
        bool has_earlier_messages = false;   // paging back will yield more
        int64_t last_loaded_seq = -1;        // cursor for the next "load older"

        // The turns the user submitted that have not yet landed as persisted
        // rows, in submit order (FIFO -- the server queue is FIFO too and
        // each entry produces exactly one text-bearing user row when its
        // turn pops). Mid-turn submits are QUEUED server-side and their seed
        // row persists only at pop, so without this echo a submitted message
        // did not render until the turn ENDED, after the assistant's
        // streaming reply. Rendered by the chat view ahead of the streaming
        // block, one queued row each; an entry retires when a page apply
        // pairs its persisted row with it (in order, past the oldest
        // baseline), when its queued turn is cancelled, or on error/session
        // end. `turn_id` is 0 until the server's TurnQueuedEvent assigns the
        // id a per-message cancel addresses.
        std::vector<PendingUserMessage> pending_user_messages;
        // Convenience for consumers that only test "is anything pending" or
        // show one line: the OLDEST pending entry's text.
        std::string pending_user_text;

        // "Is anything streaming?" -- the only thing the flat accumulated
        // text was ever good for on the snapshot path (frame_renderer.cpp's
        // fresh-session check). The flat text itself stopped being copied
        // into every deep snapshot: it duplicates live_segments' text (track
        // C4), it changed on every delta, and the per-event deep copy of a
        // growing stream was pure churn. The internal flat accumulation
        // (AppendStreamingText's block joins, the retirement matching) still
        // lives in SessionState; tests read it via FlatStreamingText().
        bool has_streaming_text = false;
        bool is_streaming_finalized = false;
        // This turn's live content in arrival order (LiveTurnSegment above).
        // Empty exactly when has_streaming_text is false AND no tool/thinking
        // segment is live (same clear sites: ClearStreaming, OnResyncRequired).
        // Each segment's text rides its copy-on-publish `text_view` (see the
        // struct) -- NOT a per-event deep copy.
        std::vector<LiveTurnSegment> live_segments;

        // Pending modals
        std::optional<PendingPermission> pending_permission;
        std::optional<PendingConfirmation> pending_confirmation;
        std::optional<PendingPluginPrompt> pending_prompt;

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

        // Bumped only by events that mean "the server has persisted rows the
        // window does not hold yet" (a tool round finished, the turn
        // completed, a resync, a rewind, a compaction) -- NOT by a page
        // apply. The refresh state machine (scrollback_refresh.hpp) keys on
        // THIS rather than on transcript_version: an apply bumps
        // transcript_version to re-render, and when "fetch needed" and
        // "re-render" shared one counter, a fetch that resolved after a newer
        // trigger marked that trigger handled too -- the turn-end fetch never
        // ran and the final round's tool rows vanished until restart.
        uint64_t fetch_generation = 0;

        // SessionState's rewind epoch (CurrentRewindEpoch): a rewind/redo
        // moved the transcript under the window, and later appends REUSE the
        // cut seqs -- anything a view cached by seq is stale when this moves.
        uint64_t rewind_epoch = 0;

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

        // The live view dropped older segments of the current turn (the
        // kMaxLiveSegments count cap latched live_view_truncated_): the
        // status bar's {activity} suffix reports it until the turn-end
        // refresh retires the covered segments. Chrome-only signal -- no
        // synthetic segment exists in the stream.
        bool live_view_truncated = false;

        // A turn is (or may be) running on this session, from the CLIENT's
        // point of view: armed by OnChatSubmitted and by the first delta of a
        // new turn (injected turns -- a subagent completion -- never go
        // through OnChatSubmitted), cleared by the same terminal events as
        // cancel_requested. This is the interrupt probe's signal, NOT a
        // display state: the derived `state` deliberately reads Idle in the
        // windows between message stops and the next provider event
        // (MaybeGoIdleLocked), during which a turn can still be alive --
        // treating that Idle as "nothing to cancel" made ESC dead and
        // ctrl+c lethal (idle-quit) for exactly as long as the provider
        // stayed silent.
        bool turn_in_flight = false;

        // --- Session metadata (the E5 emission, one per attach + on change) ---
        // The wire source for the status bar's {model} {agent}
        // {permission_mode} segments and the `max` half of {tokens}. Empty/0 =
        // unknown; the segment renders nothing, per the unknown-segment rule.
        // model/agent_preset are the EFFECTIVE values (wire truth);
        // desired_* below are the client-chosen ones still awaiting
        // convergence.
        std::string model;
        std::string agent_preset;
        std::string permission_mode;
        int64_t context_window_tokens = 0;
        // The pause state (SetPausedEvent -> SessionMetadataEvent.paused, set
        // on EVERY metadata emission -- a bool has no "absent" wire encoding,
        // so the server fills it in on the partial renames too). The status
        // bar's paused segment renders from this.
        bool paused = false;

        // The tab's DESIRED agent/model (SetDesired*), when it differs from
        // the effective metadata above the status bar shows THIS (the fresh
        // value, the user's last word) with a pending marker until the
        // confirming metadata event lands. Empty = no desired state / equal
        // to effective.
        std::string desired_model;
        std::string desired_agent_preset;
        // Same desired-state pattern for the run mode (shift+tab): the mode
        // the client asked for that has not yet been confirmed by the
        // server's metadata event (the server applies it immediately -- an
        // atomic flip -- but the confirming event still arrives on a later
        // read). The status bar shows it with the pending marker and the
        // shift+tab cycle advances from it, so two rapid presses walk the
        // cycle instead of repeating it. Empty = none pending / equal to
        // effective.
        std::string desired_permission_mode;

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
    // `include_tool_output`: the running-tools panel's live stdout tail is
    // the only consumer of ToolExecutionState::buffered_output (up to
    // tool_output_buffer_bytes_ (64 KiB default) of live stdout per tool).
    // Since the buffer became a copy-on-publish shared pointer (see the
    // struct), a fetch is cheap either way -- but false (the default) still
    // carries NO view (null), keeping the "only a caller that is actually
    // about to render the panel's live tail publishes output" rule explicit;
    // true hands out the current published view, one refcount bump per tool.
    Snapshot TakeLightSnapshot(bool include_tool_output = false);

    // The session's id ("" until SetSessionId) without a full snapshot --
    // what tab-keyed senders resolve at call time.
    std::string SessionId() const;

    // --- Session lifecycle ---
    void OnSessionCreated(std::string name);
    // Names the tab only while it has no name yet, atomically (one lock) and
    // without touching the derived state -- the subagent-spawn seed. The old
    // check-snapshot-then-OnSessionCreated pair raced the child's own reader
    // thread, and OnSessionCreated's state_ = Idle could knock a child that
    // had just started working back to Idle.
    void SeedDisplayNameIfUnset(std::string name);
    void OnSessionMetadata(const v1::SessionMetadataEvent& metadata);

    // Sessionless-tab support. A tab's SessionState is constructed before its
    // session exists (session_id "" and no metadata), so EnsureSession tells
    // the state its id once the create resolves. The /agent //model desired
    // state (SetDesired* below) seeds the status-bar fields directly -- the
    // wire metadata event only arrives once a session exists.
    void SetSessionId(std::string session_id);
    void SetDesiredModel(std::string model_ref);
    void SetDesiredAgentPreset(std::string preset_name);
    void SetDesiredPermissionMode(std::string mode);

    // The tab's desired agent/model (unresolved refs, client-chosen; empty =
    // none). Read by EnsureSession (create-options overlay) under the state's
    // own mutex.
    std::string DesiredModel() const;
    std::string DesiredAgentPreset() const;
    std::string DesiredPermissionMode() const;

    // The /-command lines that would converge the session's EFFECTIVE
    // agent/model to the desired state right now ("/agent x", "/model p/m"),
    // in the order they must run (/agent first, so the user's model survives
    // the switch -- the server's precedence rule), or empty when converged.
    // Divergence is measured against the wire metadata's effective values:
    // while none have arrived yet (fresh create, metadata in flight) nothing
    // is sent -- the create already carried the desired state, and a later
    // diff self-corrects. Idempotent: a redundant switch of an already-equal
    // value is a server-side no-op, so a second turn fired before the
    // confirming metadata event re-sending the same lines is harmless.
    std::vector<std::string> ConvergenceCommands() const;

    // --- Streaming response ---
    void AppendStreamingText(std::string_view text);
    // Thinking deltas, the mirror of AppendStreamingText for the model's
    // thinking blocks: same cap-and-truncate budget, same turn arming, a
    // dedicated flat accumulation AND its own Thinking LiveTurnSegment, so
    // reasoning renders in arrival order among text and tool segments
    // (typically as the turn's opening block) rather than as one
    // out-of-position lump.
    void AppendStreamingThinking(std::string_view text);
    void FinalizeStreamingText();

    // --- Turn boundaries (ToolExecution / TextDelta set Working/Thinking; these clear it) ---
    void OnTurnCompleted(const v1::TurnCompletedEvent& evt);
    void OnError(const std::string& error_message);
    // A local send failure for an in-flight answer (Answer*'s stream->Send()
    // returned false: the stream is closing or its bounded queue is
    // stalled) -- NOT the same as OnError's "the session is dead" meaning.
    // Surfaces the failure as a transient notice only; touches no pending_*
    // field and no state_/turn bookkeeping, so the modal the user was trying
    // to answer (and needs to retry) stays exactly as it was. A real
    // disconnect surfaces separately, through the reader thread's own
    // on_closed_/OnError path.
    void OnSendFailed(const std::string& error_message);

    // Mid-turn usage (message_start/message_delta). Keeps {tokens} live
    // during a turn; OnTurnCompleted still overwrites with the final tally.
    void OnUsage(const v1::UsageEvent& evt);

    // /-command outcome. Stored as a transient notice for the notice line;
    // never enters the chat rows. Also retires the invocation's request_id
    // from the in-flight set (OnCommandSent).
    void OnCommandResult(const v1::CommandResult& evt);

    // A /-command was sent on this session's stream and has not answered
    // yet. While any is outstanding the tab counts as interruptible
    // (Snapshot::turn_in_flight): a markdown command's generators run on the
    // session worker BEFORE its turn exists, and ESC must reach the server's
    // Cancel -- which stops them -- instead of reading the tab as idle.
    void OnCommandSent(const std::string& request_id);

    // Drops the notice line immediately, if one is showing -- a no-op
    // otherwise (still bumps nothing when there is nothing to clear, so a
    // burst of these does not spuriously invalidate handled_notice_seq).
    // Callers: submitting another command/turn, or an idle Escape -- see
    // their own comments for why each counts as "the user has moved on".
    // Returns whether there was a notice to clear -- an idle Escape needs
    // this to decide whether it just did something (consume the key) or
    // truly had nothing to do (fall through to the editor, unchanged).
    bool ClearNotice();

    // One tick toward the notice's self-expiry (kNoticeTicksDefault), called
    // from TabMaintenance::Run for whichever tab is focused. A no-op when
    // there is no notice showing, so ticking an idle tab with nothing to
    // expire costs nothing beyond the check.
    void TickNotice(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // True while any tool is mid-call (started, not yet finished). A plain
    // scan, not a copy -- unlike CopyToolsLocked, this never touches
    // buffered_output, so it is cheap enough for the render pump to poll every
    // tick (see AppState::FocusedTabHasRunningTool). What it drives: a
    // foreground tool that prints nothing for a while produced no wire
    // events, so nothing bumped UiVersion, so even the spinner froze -- this
    // is the seam that gives the pump a heartbeat while that is true.
    bool HasRunningTool() const;

    // Elapsed time of whichever running tool started most recently (the one
    // whose live clock is in the finest-grained FormatDuration bucket), or
    // nullopt if nothing is running. Same cheap scan as HasRunningTool -- the
    // pump uses this to throttle its forced-repaint cadence to whatever the
    // displayed number can actually show instead of a flat 20Hz (app.cpp).
    std::optional<std::chrono::milliseconds> FastestRunningToolElapsedMs() const;

    // Monotonic stamp of "what the running-tools panel renders changed":
    // bumped by every tool-row mutation (OnToolStarted/OnToolOutputDelta/
    // OnToolFinished/OnBackgroundTask, the turn-completed and resync sweeps,
    // and a display rename -- the panel shows each row's session name).
    // Deliberately NOT bumped by text/thinking deltas or other wire events
    // the panel does not render, which is exactly what lets the panel's
    // render cache skip its rebuild-and-deep-copy on those (frame_renderer).
    // Atomic so the render thread can poll it without taking mtx_ (the
    // bumps themselves run under mtx_ on the reader threads); a raced read
    // is harmless -- the key only gates a cache, and a missed bump means
    // the next bump's rebuild picks the change up.
    uint64_t ToolsVersion() const {
        return tools_version_.load(std::memory_order_acquire);
    }

    // The user asked to interrupt the in-flight turn. A no-op unless the tab
    // is actually mid-turn (Thinking / Working / Waiting -- Waiting counts:
    // the server's Cancel answers a parked permission/confirmation prompt
    // with a denial, so a wedged prompt is interruptible too).
    void RequestCancel();

    // Drops the accumulated streaming text. The chat view calls this after a
    // post-turn scrollback refresh, when the streamed message has become a
    // persisted row -- otherwise the same content would render twice.
    void ClearStreaming();

    // Test/debug seam: the FLAT accumulated text (AppendStreamingText's
    // block joins) under the state lock. The snapshot stopped carrying it
    // (see Snapshot::has_streaming_text -- the per-event deep copy of a
    // growing stream was churn); these hand a copy to tests that assert the
    // accumulation semantics.
    std::string FlatStreamingText() const;
    std::string FlatThinkingText() const;

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

    // Whether the LAST turn has ended and no new one has begun (Snapshot::
    // turn_ended) without a full snapshot -- what RequestScrollback captures
    // at submit time to decide its apply's retirement mode: a turn-end
    // refresh coarsely clears the covered turns' live material; a mid-turn
    // fetch (a tool round finishing while the turn continues) relies on the
    // content-matched retirement inside ApplyScrollbackPage alone.
    bool CurrentTurnEnded() const;

    // Bumped by OnSessionRewound -- what RequestScrollback captures at submit
    // time (mirroring CurrentTurnSeq above) to detect a rewind/redo that
    // happened while its fetch was in flight, so the stale response can be
    // discarded instead of re-inserting rows the rewind just cut.
    uint64_t CurrentRewindEpoch() const;

    // A GetMessages response that arrived after a rewind/redo superseded the
    // fetch that requested it: resolves the pending-fetch bookkeeping
    // (mirroring ApplyScrollbackPage's empty-page branch) WITHOUT touching
    // scrollback_page_ -- OnSessionRewound already produced the authoritative
    // window, and this response no longer describes it. Deliberately not
    // NoteRefreshFailed: nothing failed, the fetch was just superseded, and
    // that path's failure-streak notice would be spurious here.
    void DiscardStaleScrollbackPage();

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
    void SetPluginPrompt(std::unique_ptr<PendingPluginPrompt> req);
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
    // `delta` marks an after_seq response (ASCENDING rows: only what the
    // client does not hold -- see GetMessagesRequest.after_seq): the page
    // merges onto the window's newest end instead of replacing it, and
    // has_more ("more newer rows") never touches the load-older affordance.
    // A non-delta (newest-page) apply keeps its replace semantics.
    void ApplyScrollbackPage(const v1::GetMessagesResponse& resp, bool delta = false);
    bool CanLoadMore() const;

    // The window's NEWEST seq (descending window: front), or -1 when nothing
    // is held -- the delta-fetch cursor, captured without a full snapshot.
    int64_t NewestScrollbackSeq() const;

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
    // by SendChatTurn after the event was accepted onto the wire). Entries
    // retire when a page apply pairs their persisted row with them (FIFO),
    // when the server cancels their queued turn, or on error/session end,
    // which leave nothing to echo.
    void OnChatSubmitted(const std::string& text);

    // The server accepted a queued turn (TurnQueuedEvent): assigns the
    // per-session id to the OLDEST still-unassigned echo with the same text,
    // making it addressable by a per-message cancel. An event with no
    // matching echo (another client's submission) is ignored.
    void OnTurnQueued(std::uint64_t turn_id, const std::string& text);

    // A queued turn left the server's work queue without running
    // (TurnCanceledEvent): removed=true drops the matching echo (its row will
    // never exist); removed=false (already popped) keeps it -- the persisted
    // row will retire it in the normal way.
    void OnTurnCanceled(std::uint64_t turn_id, bool removed);

    // The server compacted this session's working context (CompactionEvent).
    // Records it for the chat view's compaction row and resets the status
    // bar's occupancy to the post-compaction size -- the next turn's usage
    // would otherwise be the first sign the context shrank.
    void OnCompaction(const v1::CompactionEvent& evt);

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

    // Content-matched retirement of live segments that a scrollback apply
    // has just proven persisted (mtx_ held by caller). `answered_tool_ids`
    // carries every tool_use_id whose tool_result block is in the applied
    // rows; `assistant_text` / `assistant_thinking` every text / thinking
    // block of applied assistant messages. A Tool segment retires when its
    // id is answered AND the tool is not still running (background rows
    // especially: their detached task outlives the acked result); a Text or
    // Thinking segment retires when its exact text is among the applied
    // blocks (a cap-truncated segment matches by prefix -- the marker's tail
    // is client-side presentation loss). This is what lets a fetch apply
    // MID-TURN without double-rendering: the persisted rows that just landed
    // replace exactly the live segments they cover, while anything still
    // streaming or running stays live below them.
    void RetireCoveredLiveSegments(const std::set<std::string>& answered_tool_ids,
                                   const std::vector<std::string>& assistant_text,
                                   const std::vector<std::string>& assistant_thinking);

    // Shared by every live_segments_ append path (text/thinking deltas,
    // OnToolStarted, OnBackgroundTask): enforces kMaxLiveSegments, keeping the
    // NEWEST segments. No synthetic marker is inserted -- the overflow only
    // latches live_view_truncated_, which the status bar reports. Within the
    // kept segments, Text/Thinking are trimmed BEFORE Tool markers: a marker
    // is a tiny struct, and its disappearance mid-turn is exactly the "tool
    // calls invisible until finished" failure -- the persisted rows that
    // would replace it only arrive with the refresh. (A Tool marker is only
    // trimmed when the turn is entirely tool calls. The one bug class the
    // 2026-09 permanent-sentinel design guarded -- an erase pass eating the
    // marker -- is gone with the marker; the oldest-real-segment erase order
    // it pinned is unchanged.)
    // mtx_ held by caller.
    void TrimLiveSegmentsLocked();

    // Shared by every "drop all live streaming state" site (ClearStreaming,
    // the full-clear branches of ClearStreamingIfCurrentTurn/
    // RetireCoveredLiveSegments, OnResyncRequired, OnSessionRewound): resets
    // the flat text/thinking accumulations, their truncation flags,
    // is_streaming_, live_segments_, and live_view_truncated_. mtx_ held by
    // caller.
    void ClearStreamingLocked();

    // Shared by TakeSnapshot/TakeLightSnapshot: the single place a tool row
    // is copied out to a snapshot. The working stdout buffer (up to
    // tool_output_buffer_bytes_ (64 KiB default) per tool) never leaves
    // SessionState; include_output=true publishes the copy-on-write view
    // (see ToolExecutionState::buffered_output), false carries no view.
    // ChatView::RenderLiveToolRow -- the deep snapshot's only tool consumer
    // -- reads name/started/is_error/summary, never the buffer (the
    // running-tools panel is what renders it, off the LIGHT snapshot with
    // include_tool_output=true). mtx_ held by caller.
    std::vector<ToolExecutionState> CopyToolsLocked(bool include_output) const;

    // TakeSnapshot's live_segments copy: field-copy of every segment with
    // each text-bearing segment's copy-on-publish view (see LiveTurnSegment)
    // republished only when the segment changed since the last deep
    // snapshot. The working `text` strings never leave SessionState.
    // mtx_ held by caller.
    void FillCommonLocked(Snapshot& s) const;   // caller holds mtx_
    bool ArmNewTurnLocked();                     // caller holds mtx_; see the .cpp
    void ClearPendingPromptsLocked();            // caller holds mtx_
    std::vector<LiveTurnSegment> CopyLiveSegmentsLocked() const;

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
    // See Snapshot::compactions. Deduplicated on (at_seq, created_at): a live
    // event and the page that later carries the same record are one row.
    std::vector<v1::CompactionEvent> compactions_;
    std::shared_ptr<const std::vector<v1::CompactionEvent>> compactions_shared_ =
        std::make_shared<const std::vector<v1::CompactionEvent>>();
    // Merges `incoming` into compactions_ and republishes the shared view
    // when anything changed. Caller holds mtx_.
    template <typename Range>
    void MergeCompactionsLocked(const Range& incoming);
    int64_t last_loaded_seq_ = -1;      // oldest seq loaded so far (descending)
    bool has_earlier_ = false;
    uint64_t transcript_version_ = 0;
    uint64_t fetch_generation_ = 0;   // see Snapshot::fetch_generation
    uint64_t refresh_seq_ = 0;   // see Snapshot::refresh_seq
    uint64_t rewind_epoch_ = 0;   // see CurrentRewindEpoch
    bool refresh_succeeded_ = false;   // outcome of the attempt that bumped refresh_seq_
    // Consecutive RESOLVED-FAILED scrollback fetches; reset by every
    // successful apply. Drives the one-shot error notice in NoteRefreshFailed
    // (retry pacing itself lives in scrollback_refresh.hpp's per-tab backoff).
    int scrollback_failures_ = 0;
    bool scrollback_trimmed_ = false;
    bool scrollback_gap_ = false;   // see Snapshot::scrollback_gap
    int scrollback_max_messages_ = kDefaultScrollbackWindowMaxMessages;
    int tool_output_buffer_bytes_ = kDefaultToolOutputBufferBytes;

    // The optimistic user-turn echoes (see Snapshot::pending_user_messages).
    // FIFO, paired with their persisted rows in order on page applies.
    std::vector<PendingUserMessage> pending_user_messages_;

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

    // Session metadata (see Snapshot). model_/agent_preset_ are EFFECTIVE
    // (wire truth); the desired_* pair is the client-chosen state awaiting
    // convergence (see ConvergenceCommands).
    std::string model_;
    std::string agent_preset_;
    std::string desired_model_;
    std::string desired_agent_;
    std::string permission_mode_;
    std::string desired_permission_mode_;  // shift+tab, sent but not yet confirmed
    bool paused_ = false;
    int64_t context_window_tokens_ = 0;

    // Streaming assistant text
    // CAPPED (track R2): a runaway turn must not grow client memory (or the
    // per-frame deep snapshot, which copies both members) without limit.
    // streaming_accumulated_ stops at kStreamingTextCapBytes -- further
    // deltas are dropped once a truncation marker has been appended; the
    // count of live_segments_ stops at kMaxLiveSegments (newest kept, the
    // overflow latching live_view_truncated_ for the status bar). The
    // transcript itself is authoritative: the post-turn refresh re-fetches
    // the full persisted text (scrollback_refresh.hpp).
    std::string streaming_accumulated_;
    // Live thinking accumulation (mirrors streaming_accumulated_):
    // same cap-and-truncate budget as the flat text buffer, plus the turn
    // stamp the turn-aware clear sites need to retire it only when the
    // persisted page actually covers its turn.
    std::string thinking_accumulated_;
    bool thinking_truncated_ = false;       // thinking_accumulated_ hit its cap
    uint64_t thinking_turn_seq_ = 0;        // the turn the accumulation belongs to
    bool streaming_truncated_ = false;      // streaming_accumulated_ hit its cap
    bool live_view_truncated_ = false;  // overflow dropped older live segments;
                                        // surfaced by the status bar's {activity}
    bool is_streaming_ = false;
    uint64_t turn_seq_ = 0;    // increments when a NEW TURN's stream starts
    bool turn_ended_ = true;   // armed by OnTurnCompleted/OnError; disarmed by the next delta
    // The interrupt probe's turn-liveness signal (Snapshot::turn_in_flight):
    // armed by OnChatSubmitted and by both delta arming branches (the same
    // places turn_ended_ is disarmed), cleared ONLY by the turn-terminal
    // events. Never disarmed mid-turn -- unlike turn_ended_, which flips at
    // every block boundary.
    bool turn_in_flight_ = false;
    // Request ids of /-commands sent and not yet answered (OnCommandSent /
    // OnCommandResult); cleared with turn_in_flight_ by the stream-terminal
    // events. Folded into Snapshot::turn_in_flight.
    std::set<std::string> commands_in_flight_;
    // This turn's live content in arrival order (track C4) -- see
    // Snapshot::live_segments. Same clear sites as streaming_accumulated_.
    std::vector<LiveTurnSegment> live_segments_;
    // Source for LiveTurnSegment::segment_id. Starts at 1: 0 is the
    // default-constructed "no id" value, never assigned to a real segment.
    uint64_t next_segment_id_ = 1;

    // Pending modals
    std::optional<PendingPermission> pending_permission_;
    std::optional<PendingConfirmation> pending_confirmation_;
    std::optional<PendingPluginPrompt> pending_prompt_;

    // Tool executions (parallel dispatch; keyed by tool_use_id, not position)
    std::vector<ToolExecutionState> tools_;
    // See ToolsVersion(). atomic: the render thread polls it lock-free while
    // the reader threads bump it under mtx_.
    std::atomic<uint64_t> tools_version_{0};

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
    // True for a read-only transcript tab (OpenTranscriptTab): a real session
    // id, NO stream, and every submission path refused. Exists so the
    // stream-derived "sessionless" reading a null stream normally produces
    // never triggers EnsureSession -- which would otherwise mint a brand-new
    // session and repoint this tab's session_id over the transcript being
    // viewed.
    bool view_only = false;
};

// The tab bar's filter state (tab.filter_group / tab.filter_agents): which
// subset of tabs the bar and every tab-keyed navigation action (tab.next/
// previous, tab.goto.N, group.next/previous) act on. The filtering itself is
// keybinding_dispatch.hpp's FilterTabs -- pure over TabSnapshot, unit-tested
// there; AppState only HOLDS the state, the same way focused_id_ does. Two
// views, mutually exclusive, each toggling back to All when re-pressed:
enum class TabFilter {
    All,     // every tab (the default; per-run only, never persisted)
    Group,   // the focused tab's group only (its root + its subagent children)
    Agents,  // root tabs only -- subagent children hidden
};

class AppState {
public:
    explicit AppState(std::shared_ptr<SessionClient> client);
    // Closes every tab BEFORE member destruction begins. The gRPC reader
    // threads call back into GetState under mtx_ (via each stream's
    // on_closed_), and tabs_ is destroyed BEFORE mtx_ (reverse declaration
    // order) -- an open stream at scope exit therefore races a half-destroyed
    // AppState: a corrupted mutex word reads as an eternal lock, a freed
    // session_id as a SEGV. (The close itself runs outside mtx_ -- see
    // CloseAllTabs.)
    ~AppState();

    struct TabSnapshot {
        std::string tab_id;            // the UI's tab handle (focus/close/map keys)
        std::string session_id;        // empty while the tab has no session
        bool has_session = false;      // what autocomplete / the /help overlay filter on
        std::string display_name;      // for tab bar / picker
        SessionStateKind state = SessionStateKind::Idle;
        bool is_subagent = false;
        // Read-only transcript tab (OpenTranscriptTab): a session id with no
        // stream. Note has_session stays STREAM-derived (false here) -- every
        // consumer of has_session means "can send on this tab", which is
        // exactly what a view-only tab cannot do; view_only is the
        // display-side complement for UI that wants to SAY so.
        bool view_only = false;
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

        // The tab's live view dropped older segments of its current turn
        // (SessionState's live_view_truncated_ latch, via GetAllTabs) -- the
        // status bar's {activity} suffix reports it.
        bool live_view_truncated = false;

        // The tab's session pause state (SessionMetadataEvent.paused -- the
        // confirming event for SetPausedEvent). The status bar's paused
        // segment renders from the same source.
        bool paused = false;

        // The interrupt probe's turn-liveness signal (SessionState's
        // turn_in_flight, copied through GetAllTabs): the display state above
        // reads Idle in the silent windows between message stops, so the
        // busy verdicts (TabBusyForInterrupt, AnyWorkInFlight) consult this.
        bool turn_in_flight = false;
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

    // The startup --resume path for a LIVE session: attach the session and,
    // as one tab group under it, every LIVE subagent child of its group
    // (ListSessions include_subagents, filtered by parent id; finished
    // children are skipped -- a subagent's life is one turn, so there is
    // nothing to reopen). Children spawn AFTER this call arrive the ordinary
    // way, through OnSubagentSpawned. Returns the root tab's tab_id, or ""
    // when the main attach failed (e.g. the session ended between the
    // caller's probe and this call -- the caller falls back to ResumeTab).
    std::string AttachSessionGroup(const std::string& session_id);

    // Opens a READ-ONLY transcript tab for a STORED session -- a finished
    // subagent, whose resume the server refuses ("its life was one turn of
    // its parent"): no stream is opened and no CreateSession is sent. The
    // transcript renders through the ordinary scrollback machine (a fresh
    // tab fetches its newest page on its first frame; GetMessages is keyed
    // by session id and checks database existence only), load-older pages
    // back, and focus/close need nothing stream-shaped (a null-stream close
    // is already a safe no-op). `view_only` is set so the submission paths
    // refuse instead of minting a new session over the viewed transcript.
    //   Grouping: when `parent_session_id` names a tab open here, its root
    //   ordinal is inherited (the transcript appears as a "↳" child and
    //   group-closes with the parent); otherwise the tab stands as its own
    //   root. The display name is seeded from GetSession on the RPC worker
    //   it lands). Returns the tab's tab_id ("" only if the session was
    //   already open -- the existing tab is the answer then).
    std::string OpenTranscriptTab(const std::string& session_id,
                                  const std::string& parent_session_id = {});

    // Called by the reader thread for every ServerEvent whose payload is not bound to
    // a tab the client already owns (e.g. SubagentStarted on the PARENT's stream).
    void OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt);

    // Both keyed by TAB id now (the session_id overload would be ambiguous for
    // sessionless tabs, whose session_id is "").
    void CloseTab(const std::string& tab_id);
    bool HasTab(const std::string& tab_id) const;

    // Ends each named tab's stream with an EXPLICIT EndSession{mode} --
    // DETACH: the session keeps running server-side (the close
    // confirmation's "detach", /detach-session); TERMINATE: the session and
    // everything in flight are destroyed (the confirmation's "close
    // anyway"). CloseTab's bare stream drop also detaches server-side, but
    // only by fallback; these are the paths that state their intent on the
    // wire. Tabs themselves are untouched -- callers erase them (and their
    // UI caches) via CloseTab afterward; a stream Detach already latched is
    // a no-op there. Sessionless tabs in the list contribute nothing.
    void EndTabSessions(const std::vector<std::string>& tab_ids, EndSessionMode mode);

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
    // Cancels ONE queued (accepted, not yet started) turn by its
    // TurnQueuedEvent id. Always answered by a TurnCanceledEvent for that id;
    // removed=false (already popped) is reported as a notice -- the message
    // must run.
    void SendCancelQueuedTurn(const std::string& session_id, std::uint64_t turn_id);
    // Tab-keyed variant for view helpers that hold a TAB id (a tab's
    // ChatView may exist before its session does): resolves the session at
    // click time; a sessionless tab has nothing queued, so it no-ops.
    void SendCancelQueuedTurnByTab(const std::string& tab_id, std::uint64_t turn_id);

    // permission.cycle (shift+tab): a client-native wire event, like
    // SendCancel/SendRewind -- no command, no confirmation modal (the
    // keybinding IS the confirmation). The server queues the change for its
    // worker and announces it back via SessionMetadataEvent, which is what
    // the {permission_mode} status segment refreshes from. On an accepted
    // send the requested mode becomes the tab's desired state (see
    // Snapshot::desired_permission_mode): the pending marker and the next
    // press's cycle source until that metadata event confirms it.
    void SendSetRunMode(const std::string& session_id, v1::PermissionRunMode mode);

    // session.pause (alt+p, /pause // /resume): a client-native wire event,
    // like SendSetRunMode -- no command, no confirmation (the keybinding IS
    // the confirmation). Unlike the run-mode change this applies READER-SIDE
    // (an atomic flip), so there is no turn-boundary wait and no desired-
    // state marker: the confirming SessionMetadataEvent lands within
    // milliseconds and the status bar's paused segment refreshes from it.
    void SendSetPaused(const std::string& session_id, bool paused);

    // What RequestPaused did with the focused tab's pause state.
    enum class PauseToggle {
        SentPause,        // SetPaused(true) went out; the turn stops at its
                          // next tool-call return
        SentResume,       // SetPaused(false) went out; the server enqueues the
                          // seedless continuation iff the last turn ended
                          // Paused (Session::SetPaused's resume kick)
        AlreadyPaused,    // /pause on a paused session -- nothing sent
        AlreadyRunning,   // /resume on an unpaused session -- nothing sent
        NoSession         // the focused tab has no session; nothing sent
    };

    // The /pause // /resume decision, extracted so its guard is testable:
    // the current state comes from the FOCUSED TAB's snapshot (GetDetails is
    // tab-id keyed -- the typed command must not look it up by session id,
    // which can never match a tab and always reads unpaused), while the wire
    // event targets the focused SESSION. Idempotent: a toggle whose wanted
    // state matches the current one sends nothing.
    PauseToggle RequestPaused(bool want_paused);

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

    // The same for a SESSIONLESS tab: `project_root` scopes the request
    // directly (SuggestPathsForRoot), no session row involved.
    void RequestSuggestPathsForRoot(const std::string& project_root, const std::string& prefix,
                                    int32_t limit,
                                    std::function<void(std::vector<std::string>)> on_result);

    // Asynchronously fetches the /-command catalog (ListCommands) scoped to
    // `session_id` (its own project root, server-resolved) or, when that is
    // empty, `project_root`. `on_result` runs on the RPC worker with the
    // response, or nullopt on any failure, and must not block; BumpUiVersion
    // follows so the next frame reads what it published.
    void RequestListCommands(const std::string& session_id, const std::string& project_root,
                             std::function<void(std::optional<v1::ListCommandsResponse>)> on_result);

    // --- User answers to pending prompts (UI calls these; routes ClientEvents back) ---

    // deny_message / stop_turn refine a DENY (the server ignores both on ALLOW).
    void AnswerPermission(const std::string& session_id, const std::string& request_id,
                         v1::PermissionDecision decision, std::vector<std::string> granted_keys,
                         std::string deny_message = {}, bool stop_turn = false);

    void AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                            const std::string& chosen_option);
    // `answer_json` rides only with PROMPT_OUTCOME_ANSWERED.
    void AnswerPluginPrompt(const std::string& session_id, const std::string& request_id,
                            v1::PromptOutcome outcome, const std::string& answer_json = {});

    // --- Focus (tab cycling lives in keybinding_dispatch: NextVisibleTab) ---
    void SetFocusedTab(const std::string& tab_id);

    // --- Tab-bar filter (tab.filter_group / tab.filter_agents) --------------
    // Which tabs the bar and tab-keyed navigation show (TabFilter above).
    // SetTabFilter is a no-op when the value is unchanged (no pointless
    // repaint); any real change bumps UiVersion so the pump redraws the bar.
    // Per-run only: a fresh launch starts unfiltered.
    void SetTabFilter(TabFilter filter);
    TabFilter ActiveTabFilter() const;

    // --- Desired agent/model state (/agent, /model on ANY tab) ---

    // Sets the tab's desired agent/model (unresolved refs; validated by the
    // caller -- /model refs are shape-checked client-side, preset names are
    // validated server-side at create or at the turn-start convergence).
    // The choice is the same state on every tab:
    //   * sessionless tab: rides the CreateSessionRequest when EnsureSession
    //     creates the session (the first message),
    //   * sessioned tab: converges at the NEXT TURN START, where
    //     SubmitChatTurn submits "/agent x" //"/model p/m" ahead of the turn
    //     when the desired value differs from the wire metadata's effective
    //     one -- selection never mutates a live session directly, so swapping
    //     fifteen times costs zero server traffic until a message is sent.
    // Either way the status bar reflects the choice immediately (the desired
    // pair in SessionState::Snapshot). Returns false only when the tab does
    // not exist.
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
    // Opens the view `view_id` on the focused tab, through the same path a
    // command result's view_hint takes (a VIEW-display notice the maintenance
    // pass routes once). False with no tab to carry it.
    bool RequestView(const std::string& view_id);

    // Drops the tab's notice line immediately, if one is showing. For the
    // client-native commands (bare /help, /keymap, bare /theme //agent
    // //model opening a picker) that answer by opening an overlay rather than
    // setting a fresh notice -- SubmitCommand/RunSessionlessCommand clear on
    // their own already, since they always produce a new outcome to show.
    // Returns whether there was a notice to clear; see SessionState::ClearNotice.
    bool ClearTabNotice(const std::string& tab_id);

    // One tick toward the tab's notice self-expiry; see
    // SessionState::TickNotice. Called from TabMaintenance::Run for whichever
    // tab is focused -- GetStateByTab is private, so this is the seam a view
    // helper reaches through rather than reaching into SessionState directly.
    void TickTabNotice(const std::string& tab_id);

    // --- Rendering ---
    // in render order. `include_tool_output`: see TakeLightSnapshot -- pass
    // true only when about to render the running-tools panel.
    std::vector<TabSnapshot> GetAllTabs(bool include_tool_output = false) const;

    // The focused tab's session id ("" when no tabs, or when the focused tab
    // has no session yet). The session-keyed senders target it.
    std::string FocusedSessionId() const;

    // What scopes the focused tab's /-command catalog: its session id once
    // one exists, else the project root its first message would create the
    // session with (pending_create). Both empty with no focused tab.
    struct CommandScope {
        std::string session_id;
        std::string project_root;
    };
    CommandScope FocusedCommandScope() const;

    // The focused tab's TAB id ("" when no tabs) -- the UI's handle for it,
    // valid whether or not a session exists yet.
    std::string FocusedTabId() const;

    // True when the focused tab shows a SUBAGENT session (a parent_session_id
    // on its tab info). Gates the /-palette's read-only filter: a child's
    // command surface is read-only server-side, so the client offers only
    // what would not be refused there.
    bool FocusedTabIsSubagent() const;

    // True while the focused tab has a tool mid-call. The render pump polls
    // this every tick (see app.cpp's pump lambda) to force a repaint even
    // with no wire event to bump UiVersion -- a foreground tool that goes
    // quiet for a while (a long `sleep`, a silent build step) otherwise
    // produces no events at all, freezing even the spinner. Cheap: a scan,
    // not a snapshot copy (SessionState::HasRunningTool).
    bool FocusedTabHasRunningTool() const;

    // FocusedTabHasRunningTool's elapsed-time counterpart -- see
    // SessionState::FastestRunningToolElapsedMs.
    std::optional<std::chrono::milliseconds> FocusedTabFastestRunningToolElapsedMs() const;

    // Closes (drops) every tab's stream. The ~AppState safety net, not a user
    // verb -- ctrl+w closes ONE tab (CloseTab), quit detaches (DetachAllTabs),
    // and the close confirmation's terminate path goes through
    // EndTabSessions{TERMINATE} + CloseTab.
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
    void SetChatSurface(plugins::ChatSurface* surface) {
        chat_surface_.store(surface, std::memory_order_release);
    }

    // The exclusive chat surface, unless a plugin threw out of it (catch +
    // disable): null means the core chat view renders the tab body.
    plugins::ChatSurface* EffectiveChatSurface() const {
        return chat_surface_broken_.load(std::memory_order_acquire)
                   ? nullptr
                   : chat_surface_.load(std::memory_order_acquire);
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

    // A hash of every tab's (id, SessionState::ToolsVersion) -- "did anything the
    // running-tools panel renders change on ANY tab". The panel's render
    // cache keys on this (plus the tab count and the theme epoch, see
    // frame_renderer.hpp) instead of UiVersion, so text/thinking deltas --
    // which bump UiVersion once per wire event but change nothing the panel
    // shows -- no longer trigger the panel's rebuild-and-fetch. Locks mtx_
    // (the tabs vector), like GetAllTabs next to which the caller already
    // runs; each per-tab read is a lock-free atomic load.
    uint64_t ToolsVersion() const;

    // Joins the async RPC worker (rpc_queue.hpp; RequestScrollback and
    // friends). Call once, after the render pump stops posting frames and
    // BEFORE CloseAllTabs() -- a queued job's completion callback touches
    // SessionState/AppState the same way a reader thread's dispatch does,
    // and CloseAllTabs() tears down the very tabs a straggling job would
    // publish into.
    // Also drains the stream closer (see closer_): every queued Detach/Close
    // runs before this returns, so no stream thread outlives AppState.
    void StopRpcQueue() {
        rpc_queue_.Stop();
        bulk_queue_.Stop();
        closer_.Stop();
    }

    // True if the RPC worker has a job queued or running -- callable before
    // StopRpcQueue() to decide whether its join (unbounded against whatever
    // job is currently executing) is worth warning about; see RpcQueue::Busy.
    bool RpcQueueBusy() const { return rpc_queue_.Busy(); }

    // True if any sessionless tab has actions waiting for its EnsureSession
    // to land (SubmitChatTurn/SubmitCommand/OnTabEnsureSession's queued
    // closures) -- work AnyWorkInFlight (keybinding_dispatch.hpp) can't see,
    // since that reads SessionStateKind and a freshly queued sessionless tab
    // is still Idle until its create round trip actually starts. Quit's
    // StopRpcQueue() DISCARDS the RPC queue rather than draining it (see
    // rpc_queue.hpp), so without this a message typed into a sessionless tab
    // moments before quit was silently dropped with no confirmation shown.
    bool HasQueuedWork() const;

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
    //
    // `lane`: kBulk for listing-sized work a user waits on only inside its own
    // overlay (picker fetches, ListModels' up-to-15 s discovery refresh, a
    // delete). One FIFO worker ran those ahead of the first message's
    // EnsureSession, @-mention suggestions and scrollback refreshes; the bulk
    // lane is its own worker, so a slow listing delays only other listings.
    enum class RpcLane { kInteractive, kBulk };
    void RunOnRpcQueue(std::function<void()> job, bool must_run = true,
                       RpcLane lane = RpcLane::kInteractive) {
        (lane == RpcLane::kBulk ? bulk_queue_ : rpc_queue_).Submit(std::move(job), must_run);
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

        // True from the moment the create resolves (the stream becomes
        // visible) until the drained backlog is on the wire. Gates the
        // sessioned fast paths: a submit landing in the attach window would
        // otherwise overtake the still-pending backlog, and the server --
        // receiving the LATER-queued message first -- starts ITS turn, so the
        // backlog entries arrive mid-turn and are STEERED (injected) instead
        // of running their own turns. FIFO submission order is the contract
        // the stacked-sends guarantee rests on.
        bool draining = false;
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

    // The tab-bar filter (TabFilter / ActiveTabFilter). Under mtx_ with
    // focused_id_: written by the loop thread's dispatch, read by the frame
    // -- the same single-threaded-plus-lock shape focused_id_ has.
    TabFilter tab_filter_ = TabFilter::All;

    // ui.json bounds (0 = built-in default), applied to every new SessionState.
    int scrollback_max_messages_ = 0;
    int tool_output_buffer_bytes_ = 0;

    std::atomic<uint64_t> ui_version_{0};
    void BumpUiVersion() { ui_version_.fetch_add(1, std::memory_order_acq_rel); }
    // Bumps on scope exit, i.e. AFTER the mutation the bump announces. A
    // bump at a method's entry let the pump post, and TabMaintenance latch
    // the new version, before the change landed -- that frame rendered the
    // old state and nothing re-rendered it until an unrelated event.
    struct UiVersionBumpOnExit {
        AppState& app;
        ~UiVersionBumpOnExit() { app.BumpUiVersion(); }
    };

    // T2: the async unary RPC worker backing RequestScrollback (and, over
    // time, the other calls app.cpp used to make on the loop thread). One
    // queue for the whole app, not per tab -- a single worker gives FIFO
    // ordering for free, so e.g. a queued delete can never race ahead of a
    // list queued after it.
    RpcQueue rpc_queue_;
    RpcQueue bulk_queue_;   // see RunOnRpcQueue's `lane`
    // Stream teardown off the loop thread: Stream::Close/Detach join the
    // reader and writer (Detach waits up to ~2 s each on a wedged server),
    // and CloseTab/EndTabSessions used to pay that on the UI thread -- the
    // whole TUI froze for seconds per stream in a group. The tab leaves the
    // model at once; its stream closes here, in FIFO order (a group's
    // Detach runs before its Close). Drained, not discarded, on stop.
    RpcQueue closer_{1024, /*drain_on_stop=*/true};
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
    // Atomic: SetChatSurface can race a reader thread's DispatchToTab read
    // via EffectiveChatSurface (e.g. late/dynamic plugin registration) --
    // same unsynchronized-race class chat_surface_broken_ below was already
    // fixed for.
    std::atomic<plugins::ChatSurface*> chat_surface_{nullptr};
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

    // Unlocked, mutex held by caller. Tab-keyed (the UI's primary axis).
    TabEntry* FindTab(const std::string& tab_id);
    // Session-keyed lookups: wire-event routing (events carry session_id),
    // the senders, and subagent parenting -- all only ever target tabs that
    // have a session.
    TabEntry* FindTabBySession(const std::string& session_id);
    std::shared_ptr<SessionClient::Stream> GetStream(const std::string& session_id);

    // RPC-worker only: creates the session for a sessionless tab (Stream::Open
    // with the staged create-options), registers the stream, then drains the
    // entry's pending actions. Returns the session id ("" on refusal, with the
    // error surfaced visibly on the tab).
    std::string EnsureSession(const std::string& tab_id);

    // Kicks EnsureSession onto the RPC worker (callers hold no lock).
    void RunEnsure(const std::string& tab_id);

    // How a send the stream REFUSED is reported: kError marks the tab
    // errored (OnError); kKeepPending only raises a notice (OnSendFailed),
    // leaving the prompt being answered up for a retry -- clearing it would
    // strand the server worker parked on the answer.
    enum class SendRefusal { kError, kKeepPending };
    // The shared shape of every outbound ClientEvent: resolve the session's
    // stream (a stream-less tab says so), refuse an ended session, let `fill`
    // build the event (false: nothing to send), send it, surface a refusal.
    // `what` completes "cannot <what>: ...". Bumps UiVersion on the way out.
    // True when the event went out.
    bool SendOnStream(const std::string& session_id, const std::string& what,
                      const std::function<bool(v1::ClientEvent&)>& fill,
                      SendRefusal refusal = SendRefusal::kError);
    // An answered prompt: clear it and re-derive the tab's state (the worker
    // was parked on this answer; nothing else moves the tab off Waiting).
    void ResolvePending(const std::string& session_id, const std::string& request_id);
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
