#pragma once

// The value types a session's state is made of -- tool rows, live turn
// segments, pending prompts, the derived tab state -- shared by SessionState
// (state.hpp) and its components (session_components.hpp).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "minimal/v1/common.pb.h"
#include "minimal/v1/session_service.pb.h"
#include "minimal/client/chat_model.hpp"

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

// The shared `{}` every ToolExecutionState::input starts as (never null).
inline const std::shared_ptr<const nlohmann::json>& EmptyToolInput() {
    static const auto kEmpty = std::make_shared<const nlohmann::json>(nlohmann::json::object());
    return kEmpty;
}

struct ToolExecutionState {
    std::string tool_use_id;
    std::string tool_name;
    // The tool's own one-line description (Tool::Summary via
    // ToolExecutionEvent.summary), or a background task's command. Empty when
    // the tool sends none.
    std::optional<std::string> summary;
    // The model's call args (ToolExecutionEvent.input), for SummarizeToolInput
    // when `summary` is empty. Shared and immutable, never null: every
    // snapshot (the light one per tab per frame included) copies the row, and
    // a write/edit call's input carries the whole file -- a deep copy per row
    // per snapshot was the cost. Parsed once at Start; copies bump a refcount.
    std::shared_ptr<const nlohmann::json> input = EmptyToolInput();
    std::chrono::milliseconds started_at_unix_ms{-1};  // server-supplied timestamp
    // Set by OnToolFinished from the finish event's duration_ms; -1 while the
    // tool is still running (elapsed is computed from started_at_unix_ms
    // instead -- see RenderLiveToolRow/RenderToolsPanel).
    std::chrono::milliseconds duration{-1};

    // Accumulated chunks arrive here (lossy ring buffer). The final authoritative
    // record is the persisted ToolResultBlock, but this is what the panel shows.
    //
    // SESSIONSTATE-INTERNAL working buffer, split head/tail like the server's
    // sidecar CappedBuffer: OnToolOutputDelta fills `output_head` up to the
    // head budget (cap/4) and appends the rest to `output_tail`, which is
    // compacted only once it reaches TWICE its budget -- amortized O(delta).
    // Rebuilding a cap-sized head+marker+tail string on EVERY delta past the
    // cap was a full-buffer copy per 8 KiB chunk on the gRPC reader thread.
    // The exact cap is applied at PUBLISH (ToolRunTracker::Copy), which is the
    // only reader; storage may transiently hold up to ~2x the cap. Never
    // copied out to a snapshot: `buffered_output` below is what they carry.
    std::string output_head;
    std::string output_tail;
    // Latched once head+tail first exceeded the cap: from then on the
    // published view carries the trimmed marker.
    bool output_over_cap = false;

    // The published live-stdout view: WHAT SNAPSHOTS CARRY. Null unless the
    // snapshot was taken with include_tool_output (only the running-tools
    // panel renders it). Shared, immutable -- ToolRunTracker::Copy(true) hands
    // every consumer the SAME pointer until the buffer next changes, so a
    // rebuild of the panel (or any other include-output fetch) no longer
    // deep-copies up to tool_output_buffer_bytes_ (64 KiB default) per tool
    // per fetch; it pays one refcount bump.
    // mutable: publishing (rebuilding this pointer from the working buffer)
    // happens inside ToolRunTracker::Copy -- const, called from TakeLightSnapshot
    // -- the same cache-write-on-read rule as output_dirty below.
    mutable std::shared_ptr<const std::string> buffered_output;
    // Copy-on-publish stamp: set by every buffered_output_working mutation,
    // cleared when the pointer above is (re)published. mutable: publishing
    // happens inside ToolRunTracker::Copy (const, called from TakeLightSnapshot).
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
    // segments are REMOVED mid-turn (LiveTurnBuffer::Retire's page apply,
    // LiveTurnBuffer's count cap, ClearStreamingIfCurrentTurn) and
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
    // like the tool-output cache inside ToolRunTracker::Copy.
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

}  // namespace minimal::client
