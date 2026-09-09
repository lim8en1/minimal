#include "minimal/client/state.hpp"

#include "minimal/client/plugins/registries.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <utility>

#include "minimal/client/cli_args.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {
// Appended ONCE when a streaming buffer (flat accumulation or a single live
// segment) hits SessionState::kStreamingTextCapBytes; further deltas are
// dropped after it -- the transcript is authoritative, the turn-end refresh
// re-fetches the full persisted text.
constexpr std::string_view kStreamingTruncationMarker =
    "\n··· live text truncated -- the full text is in the transcript ···\n";
}  // namespace

// ===========================================================================
// SessionState — one per tab, thread-safe snapshot surface.
// All mutators lock an internal mutex (never held across RPC/IO), so they are
// safe to call from the reader thread while AppState owns its own ordering.
// ===========================================================================

SessionState::SessionState(std::string session_id, std::string name)
    : session_id_(std::move(session_id)), display_name_(std::move(name)) {}

std::vector<ToolExecutionState> SessionState::CopyToolsLocked(bool include_output) const {
    if (include_output) return tools_;   // bounded (R2); the tools panel renders these
    // Field-by-field, NOT `tool; tool.buffered_output.clear()` -- that would
    // copy the (up to 64 KiB) buffer first and clear it after, paying exactly
    // the cost this exists to avoid. Every other field is cheap (small
    // strings, a timestamp, two bools).
    std::vector<ToolExecutionState> light;
    light.reserve(tools_.size());
    for (const auto& tool : tools_) {
        ToolExecutionState t;
        t.tool_use_id = tool.tool_use_id;
        t.tool_name = tool.tool_name;
        t.summary = tool.summary;
        t.started_at_unix_ms = tool.started_at_unix_ms;
        t.started = tool.started;
        t.is_error = tool.is_error;
        t.is_background = tool.is_background;
        light.push_back(std::move(t));
    }
    return light;
}

SessionState::Snapshot SessionState::TakeLightSnapshot(bool include_tool_output) {
    std::lock_guard lk(mtx_);
    Snapshot s;
    s.session_id = session_id_;
    s.display_name = display_name_;
    s.state = state_;
    s.tools = CopyToolsLocked(include_tool_output);
    s.input_tokens = input_tokens_;
    s.output_tokens = output_tokens_;
    s.cache_read_tokens = cache_read_tokens_;
    s.cache_write_tokens = cache_write_tokens_;
    s.cancel_requested = cancel_requested_;
    s.model = model_;
    s.agent_preset = agent_preset_;
    s.permission_mode = permission_mode_;
    s.context_window_tokens = context_window_tokens_;
    // Deliberately NOT copied (heavy, and only the focused tab renders them):
    // scrollback_page, streaming_text, streaming_thinking_text,
    // pending_permission/confirmation, latest_notice.
    return s;
}

void SessionState::SetBounds(int scrollback_max_messages, int tool_output_buffer_bytes) {
    std::lock_guard lk(mtx_);
    scrollback_max_messages_ = scrollback_max_messages;
    tool_output_buffer_bytes_ = tool_output_buffer_bytes;
}

SessionState::Snapshot SessionState::TakeSnapshot() {
    std::lock_guard lk(mtx_);
    Snapshot s;
    s.session_id = session_id_;
    s.display_name = display_name_;
    s.state = state_;
    s.scrollback_page = scrollback_page_shared_;   // shared, copy-on-apply
    s.pending_user_text = pending_user_text_;
    s.has_earlier_messages = has_earlier_;
    s.last_loaded_seq = last_loaded_seq_;
    s.streaming_text = streaming_accumulated_;
    s.streaming_thinking_text = thinking_accumulated_;
    s.is_streaming_finalized = !is_streaming_;   // not streaming => finalized this turn
    s.live_segments = live_segments_;
    if (pending_permission_) {
        s.pending_permission = *pending_permission_;
    }
    if (pending_confirmation_) {
        s.pending_confirmation = *pending_confirmation_;
    }
    // LIGHT tool copy: ChatView::RenderLiveToolRow (the only deep-snapshot
    // tool reader) never reads buffered_output -- the running-tools panel is what
    // renders that, off TakeLightSnapshot(true) instead. This used to copy
    // every tool's full (up to 64 KiB) live buffer on every focused-tab deep
    // snapshot for a consumer that ignores it.
    s.tools = CopyToolsLocked(/*include_output=*/false);
    s.input_tokens = input_tokens_;
    s.output_tokens = output_tokens_;
    s.cache_read_tokens = cache_read_tokens_;
    s.cache_write_tokens = cache_write_tokens_;
    s.cancel_requested = cancel_requested_;
    s.transcript_version = transcript_version_;
    s.turn_ended = turn_ended_;
    s.scrollback_trimmed = scrollback_trimmed_;
    s.scrollback_gap = scrollback_gap_;
    s.turn_seq = turn_seq_;
    s.latest_notice = latest_notice_;
    s.notice_seq = notice_seq_;
    s.model = model_;
    s.agent_preset = agent_preset_;
    s.permission_mode = permission_mode_;
    s.context_window_tokens = context_window_tokens_;
    s.refresh_seq = refresh_seq_;
    s.refresh_succeeded = refresh_succeeded_;
    return s;
}

void SessionState::OnSessionCreated(std::string name) {
    std::lock_guard lk(mtx_);
    if (!name.empty()) display_name_ = std::move(name);
    state_ = SessionStateKind::Idle;
}

void SessionState::OnSessionMetadata(const v1::SessionMetadataEvent& metadata) {
    std::lock_guard lk(mtx_);
    if (!metadata.name().empty()) {
        display_name_ = metadata.name();
    }
    // The E5 metadata fields: the status bar's wire source. Empty/0 leaves the
    // previous value alone -- a partial re-emit (a rename event) must not wipe
    // fields it does not carry.
    if (!metadata.model().empty()) model_ = metadata.model();
    if (!metadata.agent_preset().empty()) agent_preset_ = metadata.agent_preset();
    if (!metadata.permission_mode().empty()) permission_mode_ = metadata.permission_mode();
    if (metadata.context_window_tokens() > 0) {
        context_window_tokens_ = metadata.context_window_tokens();
    }
}

void SessionState::SetSessionId(std::string session_id) {
    std::lock_guard lk(mtx_);
    session_id_ = std::move(session_id);
}

void SessionState::StageDisplayModel(std::string model_ref) {
    std::lock_guard lk(mtx_);
    model_ = std::move(model_ref);
}

void SessionState::StageDisplayAgentPreset(std::string preset_name) {
    std::lock_guard lk(mtx_);
    agent_preset_ = std::move(preset_name);
}

void SessionState::AppendStreamingText(std::string_view text) {
    std::lock_guard lk(mtx_);
    // Track C4: a NEW live segment starts on a fresh turn (live_segments_
    // empty), a block boundary (is_streaming_ false -- the previous block's
    // content_block_stop already finalized it), or when the most recent
    // live entry is a TOOL marker (text resumed after a tool call). This is
    // what lets the render path show this turn's text and tool activity
    // interleaved in arrival order, instead of every tool row appearing
    // after ALL the text regardless of how far apart the model actually
    // put them (ChatView used to render one flat text block, then every
    // live tool row, unconditionally last).
    const bool new_segment = live_segments_.empty() || !is_streaming_ ||
                             live_segments_.back().kind != LiveTurnSegment::Kind::Text;
    // The turn this segment belongs to: turn_ended_ still true means THIS
    // append is the new turn's first -- it bumps turn_seq_ below, so the
    // segment must already carry the bumped value.
    const uint64_t segment_turn = turn_ended_ ? turn_seq_ + 1 : turn_seq_;
    if (new_segment) {
        live_segments_.push_back(
            LiveTurnSegment{.kind = LiveTurnSegment::Kind::Text, .turn_seq = segment_turn});
    }
    live_segments_.back().text.append(text);
    // Per-segment cap: same 2 MiB budget as the flat accumulation, same
    // shape (truncate once, append the marker, drop further deltas -- the
    // transcript is authoritative, the turn-end refresh re-fetches).
    if (!live_segments_.back().text_truncated &&
        live_segments_.back().text.size() > kStreamingTextCapBytes) {
        live_segments_.back().text.resize(kStreamingTextCapBytes);
        live_segments_.back().text.append(kStreamingTruncationMarker);
        live_segments_.back().text_truncated = true;
    }
    // Segment-COUNT cap: a turn with hundreds of tool round trips would
    // otherwise grow live_segments_ (and the per-frame deep snapshot)
    // without limit. Keep the NEWEST segments; on first overflow leave one
    // marker segment at the oldest position so the cut is visible. A marker
    // stamped turn_seq 0 is retired first by ClearStreamingIfCurrentTurn
    // (0 <= any captured turn), which is exactly right: the refresh that
    // covers the turn replaces it with persisted rows.
    if (live_segments_.size() > kMaxLiveSegments) {
        const auto keep_from = live_segments_.begin() +
                               static_cast<std::ptrdiff_t>(live_segments_.size() - kMaxLiveSegments);
        live_segments_.erase(live_segments_.begin(), keep_from);
        if (!live_segments_truncated_) {
            live_segments_.insert(live_segments_.begin(),
                                  LiveTurnSegment{.kind = LiveTurnSegment::Kind::Text,
                                                  .turn_seq = 0,
                                                  .text = "[...truncated]"});
            live_segments_truncated_ = true;
        }
    }

    // A NEW TURN begins only after the previous one ENDED (TurnCompleted/Error)
    // -- message stops fire between tool cycles WITHIN one turn, so arming on
    // is_streaming_ alone would re-key the stream mid-turn. (See turn_seq in
    // the Snapshot.)
    // A new BLOCK within the same turn: the previous block's
    // content_block_stop already finalized (is_streaming_ false), but the
    // TURN itself has not ended (turn_ended_ false) -- text -> tool -> text,
    // or thinking -> text. Without a separator the two blocks' raw markdown
    // fuses into one document (an unclosed fence from block one would eat
    // block two whole); a blank line is the same break ComposeDocument
    // already puts between rendered blocks (markdown_renderer.cpp).
    if (!turn_ended_ && !is_streaming_ && !streaming_accumulated_.empty() &&
        !streaming_truncated_) {
        streaming_accumulated_.append("\n\n");
    }
    if (turn_ended_) {
        ++turn_seq_;
        turn_ended_ = false;
        // The turn-end REFRESH (async) owns clearing the previous turn's
        // accumulated tail via ClearStreamingIfCurrentTurn. If this delta is
        // the FIRST of the new turn and wins that race, appending onto the
        // old tail fused the two turns into one document with no separator
        // -- markdown from the old turn eats this one whole. Clear now: the
        // old tail reappears from the store when the refresh lands, which is
        // authoritative anyway.
        streaming_accumulated_.clear();
        streaming_truncated_ = false;
    }
    // Flat accumulation cap: further deltas are DROPPED once the truncation
    // marker is in place (appending them past the cap would make the cap
    // decorative and re-grow the per-frame snapshot cost). The transcript
    // is authoritative -- the turn-end refresh re-fetches the full text.
    if (streaming_truncated_) {
        // already capped: skip the delta (the marker below is what renders)
    } else {
        streaming_accumulated_.append(text);
        if (streaming_accumulated_.size() > kStreamingTextCapBytes) {
            streaming_accumulated_.resize(kStreamingTextCapBytes);
            streaming_accumulated_.append(kStreamingTruncationMarker);
            streaming_truncated_ = true;
        }
    }
    is_streaming_ = true;

    // A fresh assistant turn supersedes any unanswered prompt (including Waiting
    // -- the prompt below is cleared by this same call, so the state must move
    // with it rather than being stranded in Waiting with nothing pending).
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Thinking;
    }
    pending_permission_.reset();
    pending_confirmation_.reset();
}

void SessionState::FinalizeStreamingText() {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    MaybeGoIdleLocked();
}

// Thinking deltas (ThinkingDeltaEvent) get the same budget and turn arming
// text deltas get, but a SEPARATE flat accumulation: live_segments_ model
// only Text and Tool segments (that is what the persisted rows render as),
// and the view layer needs thinking available independently anyway. Without
// this the deltas were dropped outright -- thinking materialized only in the
// turn-end refetch, a visible content jump mid-conversation.
void SessionState::AppendStreamingThinking(std::string_view text) {
    std::lock_guard lk(mtx_);
    // Flat cap, same shape as streaming_accumulated_: truncate once, append
    // the marker, drop further deltas -- the transcript is authoritative.
    if (thinking_truncated_) return;
    thinking_accumulated_.append(text);
    if (thinking_accumulated_.size() > kStreamingTextCapBytes) {
        thinking_accumulated_.resize(kStreamingTextCapBytes);
        thinking_accumulated_.append(kStreamingTruncationMarker);
        thinking_truncated_ = true;
    }
    // Turn arming mirrors AppendStreamingText exactly: thinking typically
    // opens the turn (before the first text delta), so it must bump turn_seq_
    // and stamp itself with the SAME value text's later segments will carry,
    // or ClearStreamingIfCurrentTurn would retire (or keep) the two halves
    // inconsistently.
    if (turn_ended_) {
        ++turn_seq_;
        turn_ended_ = false;
        // Same new-turn race as AppendStreamingText: a first thinking delta
        // must not append onto the previous turn's thinking tail before the
        // async refresh clears it.
        thinking_accumulated_.clear();
        thinking_truncated_ = false;
    }
    thinking_turn_seq_ = turn_seq_;
    is_streaming_ = true;
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Thinking;
    }
}

// "Idle | MessageStopEvent, nothing pending" (ARCHITECTURE.md, Tab states and
// attention). A tool-using turn emits several message_stops before the turn is
// actually done -- this fires between tool cycles too, which is correct: the
// tab really is briefly idle there.
void SessionState::MaybeGoIdleLocked() {
    if (state_ == SessionStateKind::Error || state_ == SessionStateKind::Done) return;
    // ANY tool still running, not just the LAST-started one: ARCHITECTURE.md
    // "Tool dispatch is parallel" makes several tools in flight at once the
    // COMMON case (width 4 by default), and results arrive in COMPLETION
    // order, not gate order -- tools_.back() is the last one the model asked
    // for, not the last one still running. `tools_.back().started` alone
    // flipped the tab to Idle (spinner gone, "Working" lost) the moment the
    // last-STARTED tool finished, even with earlier calls of the same round
    // still executing on the pool.
    const bool tool_active =
        std::any_of(tools_.begin(), tools_.end(), [](const ToolExecutionState& t) {
            return t.started;
        });
    if (!tool_active && !pending_permission_ && !pending_confirmation_) {
        state_ = SessionStateKind::Idle;
    }
}

void SessionState::OnTurnCompleted(const v1::TurnCompletedEvent& evt) {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    turn_ended_ = true;
    cancel_requested_ = false;   // the interrupted turn ended -- whatever the stop reason
    input_tokens_ = evt.usage().input_tokens();
    output_tokens_ = evt.usage().output_tokens();
    cache_read_tokens_ = evt.usage().cache_read_input_tokens();
    cache_write_tokens_ = evt.usage().cache_creation_input_tokens();
    // Turn completion is bookkeeping, not a state transition: `Done` means the
    // SESSION ended (see OnSessionEnded), not that a turn finished -- the
    // preceding message_stop already resolved state_ (normally to Idle). An
    // in-flight Error is left alone rather than silently cleared here.
    //
    // The panel resets when a new turn begins -- EXCEPT a still-running
    // background task's row (ARCHITECTURE.md "Background bash rows"): its
    // ToolExecutionEvent{finished} already acked the launching call, so it is
    // no longer a live FOREGROUND tool by the time any turn completes, but
    // the detached process keeps running and its terminal BackgroundTaskEvent
    // can arrive many turns later. A blanket clear here silently dropped that
    // row out from under the still-running task. A finished background row
    // (its own terminal event already closed it) is swept here exactly like
    // any other finished row -- only a LIVE one survives.
    tools_.erase(std::remove_if(tools_.begin(), tools_.end(),
                                [](const ToolExecutionState& t) {
                                    return !(t.is_background && t.started);
                                }),
                tools_.end());
    // A CANCELLED turn (or one that died at a prompt) ends WITHOUT a message
    // stop: the server denied the parked prompt on our behalf
    // (Session::Cancel's pending.CancelAll), so the answer never traveled
    // through AnswerPermission/AnswerConfirmation and the client-side pending
    // request would outlive its turn -- stranding the tab in Waiting with
    // nothing left to answer. A turn cannot complete while its OWN prompt is
    // genuinely outstanding (the worker is blocked on it), so anything pending
    // here is stale. MaybeGoIdleLocked resolves the Waiting the missing
    // message stop would have; it leaves an in-flight Error alone.
    pending_permission_.reset();
    pending_confirmation_.reset();
    MaybeGoIdleLocked();
    // The completed turn's messages are persisted now but NOT yet in the
    // scrollback window; bumping the version is what makes the chat view
    // re-fetch and swap its streaming block for the real rows.
    ++transcript_version_;
}

void SessionState::OnUsage(const v1::UsageEvent& evt) {
    std::lock_guard lk(mtx_);
    // Mid-turn liveness for {tokens}: providers emit usage at message start
    // (the input/context half) and at message_delta (the output half). The
    // turn-end OnTurnCompleted overwrite still wins as the authoritative
    // final accounting; this only keeps the segment from sitting empty for
    // the whole turn.
    input_tokens_ = evt.usage().input_tokens();
    output_tokens_ = evt.usage().output_tokens();
    cache_read_tokens_ = evt.usage().cache_read_input_tokens();
    cache_write_tokens_ = evt.usage().cache_creation_input_tokens();
}

void SessionState::OnCommandResult(const v1::CommandResult& evt) {
    std::lock_guard lk(mtx_);
    // The FULL mapper, not a hand-copied subset: dropping display/view_hint
    // here silently downgraded every View-display result (e.g. /sessions) to a
    // status line, dead on arrival.
    latest_notice_ = NoticeFromCommandResult(evt);
    ++notice_seq_;
}

void SessionState::RequestCancel() {
    std::lock_guard lk(mtx_);
    switch (state_) {
        case SessionStateKind::Thinking:
        case SessionStateKind::Working:
        case SessionStateKind::Waiting:
            cancel_requested_ = true;   // held until the turn actually ends
            break;
        default:
            break;   // nothing in flight to interrupt
    }
}

void SessionState::ClearStreaming() {
    std::lock_guard lk(mtx_);
    streaming_accumulated_.clear();
    streaming_truncated_ = false;   // fresh accumulation: cap budget resets
    thinking_accumulated_.clear();   // thinking retires with the text it preceded
    thinking_truncated_ = false;
    is_streaming_ = false;
    live_segments_.clear();
    live_segments_truncated_ = false;
}

void SessionState::ClearStreamingIfCurrentTurn(uint64_t turn_seq) {
    std::lock_guard lk(mtx_);
    // Retire the live material of the turns the persisted page covers (seq <=
    // the captured turn) -- keeping anything a NEWER turn has streamed since
    // the fetch was submitted. A blanket clear here was the stale-clear bug:
    // the user's next turn lost its opening deltas. A blanket KEEP was equally
    // wrong: the retired turn's live text would double-render against the
    // rows that now cover it. turn_seq only grows, so "capture > current"
    // cannot happen; capture == current retires everything (the common
    // turn-ended case).
    const auto keep_end = std::stable_partition(
        live_segments_.begin(), live_segments_.end(),
        [turn_seq](const LiveTurnSegment& segment) { return segment.turn_seq > turn_seq; });
    if (keep_end == live_segments_.begin()) {
        // Nothing survives: the classic clear.
        streaming_accumulated_.clear();
        streaming_truncated_ = false;
        thinking_accumulated_.clear();
        thinking_truncated_ = false;
        is_streaming_ = false;
        live_segments_.clear();
        live_segments_truncated_ = false;
        return;
    }
    // The thinking accumulation is turn-stamped (see AppendStreamingThinking):
    // retire it exactly when the persisted page covers its turn, keep it for
    // a newer turn that started since -- the same rule the segments above get.
    if (thinking_turn_seq_ <= turn_seq) {
        thinking_accumulated_.clear();
        thinking_truncated_ = false;
    }
    // Rebuild the flat accumulation from the surviving text segments (the
    // blank-line break mirrors the within-turn block separator above).
    std::string rebuilt;
    for (auto it = live_segments_.begin(); it != keep_end; ++it) {
        if (it->kind != LiveTurnSegment::Kind::Text || it->text.empty()) continue;
        if (!rebuilt.empty()) rebuilt.append("\n\n");
        rebuilt.append(it->text);
    }
    streaming_accumulated_ = std::move(rebuilt);
    // The rebuild already drops whatever pushed the old flat buffer past the
    // cap (retired segments go with their turn), so the budget is fresh.
    streaming_truncated_ =
        streaming_accumulated_.size() > kStreamingTextCapBytes;
    live_segments_.erase(keep_end, live_segments_.end());
}

uint64_t SessionState::CurrentTurnSeq() const {
    std::lock_guard lk(mtx_);
    return turn_seq_;
}

void SessionState::OnError(const std::string& error_message) {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    turn_ended_ = true;   // the errored turn is over; the next stream is a new one
    cancel_requested_ = false;   // a cancel can BE the error path (dead stream)
    pending_permission_.reset();
    pending_confirmation_.reset();
    // The turn failed before any page apply could retire the echo; nothing
    // will, so drop it here rather than leaving a ghost of an unacknowledged
    // message above the error.
    pending_user_text_.clear();
    state_ = SessionStateKind::Error;
    // The message is part of the outcome: dropped here, an errored tab showed
    // only a state tint and "cannot send"/"connection lost"/"resume failed"
    // explained nothing -- the failure read as the harness ignoring input.
    // The notice line is the established transient surface; an error notice
    // renders there in the error color, and its default STATUS display runs
    // no view side-effects (TabMaintenance::HandleNotice).
    latest_notice_ = CommandNotice{error_message, /*is_error=*/true,
                                   v1::COMMAND_DISPLAY_STATUS, /*view_hint=*/{}};
    ++notice_seq_;
}

// The real "subagent session stopped" signal (ARCHITECTURE.md): the session's
// stream ended, not merely a turn. Terminal -- nothing can answer a pending
// modal once the session is gone.
void SessionState::OnSessionEnded() {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    turn_ended_ = true;   // terminal: a final newest-fetch may still swap the partial stream
    cancel_requested_ = false;
    pending_permission_.reset();
    pending_confirmation_.reset();
    pending_user_text_.clear();   // same rule as OnError: nothing will retire it
    state_ = SessionStateKind::Done;
}

bool SessionState::IsEnded() const {
    std::lock_guard lk(mtx_);
    return state_ == SessionStateKind::Done;
}

void SessionState::OnResyncRequired(const std::string& reason) {
    std::lock_guard lk(mtx_);
    // The streamed material on hand may be missing pieces -- drop the live
    // half (the persisted transcript has the authoritative record) and mark
    // the content changed so the frame pump's newest-slice refresh fires. A
    // pending prompt is NOT dropped: the server replays outstanding prompts
    // on attach (S5.3), and a prompt whose modal is showing is still real.
    streaming_accumulated_.clear();
    streaming_truncated_ = false;
    thinking_accumulated_.clear();
    thinking_truncated_ = false;
    is_streaming_ = false;
    live_segments_.clear();
    live_segments_truncated_ = false;
    ++transcript_version_;
    common::DefaultLogger().info("session {}: resync required ({})", session_id_, reason);
}

// SessionRewoundEvent (proto/minimal/v1): the transcript tail was cut (rewind)
// or restored (redo). The durable record moved under the window -- trim it to
// the ACTIVE transcript and drop the live/pending-turn state that belonged to
// the discarded rows. Not a turn terminator, so state_ is left alone.
void SessionState::OnSessionRewound(int64_t truncated_to_seq, int64_t active_last_seq) {
    std::lock_guard lk(mtx_);
    if (truncated_to_seq < 0) {
        // REDO: the restored rows are unknown client-side -- clear the whole
        // window so the refresh refetches them from the top.
        scrollback_page_.clear();
        has_earlier_ = true;
    } else {
        // Descending window: the cut rows sit at the FRONT.
        while (!scrollback_page_.empty() && scrollback_page_.front().seq() > active_last_seq) {
            scrollback_page_.erase(scrollback_page_.begin());
        }
        // Conservative: rows survived -> older pages may exist; setting true
        // when they don't just costs one empty "load older" round trip.
        has_earlier_ = !scrollback_page_.empty() ? true : has_earlier_;
    }
    last_loaded_seq_ = scrollback_page_.empty() ? -1 : scrollback_page_.back().seq();
    // The window was rebuilt wholesale: any previously-detected stitching gap
    // describes rows that no longer exist in it.
    scrollback_gap_ = false;
    // The live-state wipe ClearStreaming does (inlined: it locks, we hold mtx_),
    // plus the pending-turn bookkeeping -- the rewound turn's echo has nothing
    // left to retire it and a pending cancel belongs to the discarded turn.
    streaming_accumulated_.clear();
    streaming_truncated_ = false;
    thinking_accumulated_.clear();
    thinking_truncated_ = false;
    is_streaming_ = false;
    live_segments_.clear();
    live_segments_truncated_ = false;
    pending_user_text_.clear();
    cancel_requested_ = false;
    scrollback_page_shared_ =
        std::make_shared<const std::vector<v1::StoredMessage>>(scrollback_page_);
    // Same bookkeeping ApplyScrollbackPage resolves with: the version bump is
    // the mismatch the refresh state machine refetches; the refresh_seq bump
    // retires any fetch that was in flight across the rewind.
    refresh_succeeded_ = true;
    ++transcript_version_;
    ++refresh_seq_;
}

// Pending modals -------------------------------------------------------------

void SessionState::SetPermissionRequest(std::unique_ptr<PendingPermission> req) {
    std::lock_guard lk(mtx_);
    pending_permission_ = std::move(*req);
    pending_confirmation_.reset();
    // Waiting from ANY active-turn state: mid-turn prompts used to arrive
    // with the tab in Working/Thinking (a tool row, or streamed text, had
    // already moved it off Idle) and left it there -- the modal still showed,
    // but the tab read as busy rather than blocked. Only Error outranks a
    // prompt: a failed turn has nothing to wait on.
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Waiting;
    }
}

void SessionState::SetConfirmationRequest(std::unique_ptr<PendingConfirmation> req) {
    std::lock_guard lk(mtx_);
    pending_confirmation_ = std::move(*req);
    pending_permission_.reset();
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Waiting;
    }
}

bool SessionState::ClearPendingFor(std::string_view request_id) {
    std::lock_guard lk(mtx_);
    bool cleared = false;
    if (pending_permission_ && pending_permission_->request_id == request_id) {
        pending_permission_.reset();
        cleared = true;
    }
    if (pending_confirmation_ && pending_confirmation_->request_id == request_id) {
        pending_confirmation_.reset();
        cleared = true;
    }
    return cleared;
}

void SessionState::OnPendingResolved() {
    std::lock_guard lk(mtx_);
    // The worker was parked on the prompt this just answered, so nothing else
    // is in flight and MaybeGoIdleLocked's guards all hold.
    MaybeGoIdleLocked();
}

// Tools ---------------------------------------------------------------------
// Tool dispatch is parallel (ARCHITECTURE.md "Tool dispatch is parallel",
// width 4 by default): several tools of one assistant round can be RUNNING
// at once, and their start/delta/finish events interleave and resolve in
// COMPLETION order, not gate order. Every handler here keys on tool_use_id
// (never on position) for exactly that reason -- multi-row is the common
// case, not just subagents and long bash commands.

void SessionState::OnToolStarted(const v1::ToolExecutionEvent& evt) {
    std::lock_guard lk(mtx_);
    // A duplicate started (a redelivery from a stream replay) must not open a
    // second row and a second live segment: the reverse-scan finisher would
    // resolve only the copy, stranding the original in "running" forever --
    // and MaybeGoIdleLocked would wait on it. Ignore the redelivery.
    for (const auto& existing : tools_) {
        if (existing.tool_use_id == evt.tool_use_id() && existing.started) return;
    }
    ToolExecutionState tool;
    tool.tool_use_id = evt.tool_use_id();
    tool.tool_name = evt.tool_name();
    tool.summary = evt.summary();
    tool.started_at_unix_ms = std::chrono::milliseconds(evt.started_at_unix_ms());
    tools_.push_back(std::move(tool));
    // Track C4: a marker in the SAME arrival-order timeline text segments
    // use, so the live view can render this tool call between the text
    // that preceded and followed it instead of after everything.
    live_segments_.push_back(LiveTurnSegment{.kind = LiveTurnSegment::Kind::Tool,
                                             .turn_seq = turn_seq_,
                                             .tool_use_id = evt.tool_use_id()});

    auto& live = tools_.back();   // the row just pushed, regardless of how many precede it
    live.started = true;
    if (state_ == SessionStateKind::Idle || state_ == SessionStateKind::Done) {
        state_ = SessionStateKind::Working;
    }
}

void SessionState::OnToolOutputDelta(const std::string& tool_use_id, const std::string& chunk) {
    if (chunk.empty()) return;   // ignore empty writes to avoid a spurious space
    std::lock_guard lk(mtx_);
    for (auto it = tools_.rbegin(); it != tools_.rend(); ++it) {
        if (it->tool_use_id == tool_use_id && it->started) {   // the active execution, if any
            it->buffered_output.append(chunk);
            // Cap AT APPEND (track R2): the "ring buffer" the header promised.
            // Render-time truncation alone let a chatty tool (a build, a
            // `tail -f`) grow client memory for the whole turn. Head ¼ +
            // marker + tail ¾, mirroring the server's persisted-result cap.
            //
            // Kept as an EXACT per-delta bound (not amortized/hysteresis): an
            // earlier attempt at this review only rebuilt once the buffer
            // reached double the cap, which broke the documented invariant
            // (session_state_test.cpp, "tool output deltas stay bounded") that
            // buffered_output stays within cap + a small marker slack at
            // every snapshot, not just eventually -- the running-tools panel
            // renders mid-stream, so a transient 2x overshoot is visible, not
            // theoretical. The real fix for the many-small-chunks cost (a
            // proper ring buffer, or an "already truncated" fast path that
            // erases only the newly-stale tail instead of rebuilding head+
            // marker+tail from three fresh strings) needs a shape change to
            // ToolExecutionState and is left as follow-up rather than risked
            // here against a passing invariant test.
            if (it->buffered_output.size() > static_cast<std::size_t>(tool_output_buffer_bytes_)) {
                static const std::string kMarker =
                    "\n··· live output trimmed -- the full result is in the transcript ···\n";
                const std::size_t head_bytes = static_cast<std::size_t>(tool_output_buffer_bytes_) / 4;
                // Defensive floor: ui_config.cpp now rejects a configured
                // bound below the marker's own size, but this is the site
                // that would actually underflow (head_bytes + marker could
                // still exceed a bound set some other way -- direct
                // construction, a future caller) -- size_t wraparound turned
                // `size() - tail_budget` into an out-of-range substr() thrown
                // on the gRPC reader thread. Falling back to a plain
                // keep-the-tail truncation is always safe.
                if (head_bytes + kMarker.size() >= static_cast<std::size_t>(tool_output_buffer_bytes_)) {
                    const std::size_t keep = static_cast<std::size_t>(tool_output_buffer_bytes_);
                    it->buffered_output = it->buffered_output.substr(
                        it->buffered_output.size() > keep ? it->buffered_output.size() - keep : 0);
                } else {
                    const std::size_t tail_budget = static_cast<std::size_t>(tool_output_buffer_bytes_) -
                                                    head_bytes - kMarker.size();
                    it->buffered_output =
                        it->buffered_output.substr(0, head_bytes) + kMarker +
                        it->buffered_output.substr(it->buffered_output.size() - tail_budget);
                }
            }
            return;
        }
    }
}

void SessionState::OnToolFinished(const std::string& tool_use_id, const v1::ToolExecutionEvent& final_evt) {
    std::lock_guard lk(mtx_);
    for (auto it = tools_.rbegin(); it != tools_.rend(); ++it) {
        if (it->tool_use_id == tool_use_id && it->started) {   // finish the matching active tool
            it->is_error = final_evt.has_is_error() ? final_evt.is_error() : false;
            // A BACKGROUND row's launching call finishing does NOT mean the
            // task itself is done -- it means the SYNCHRONOUS half (the gate
            // + the call that kicked the detached process off) returned; the
            // process keeps running (ARCHITECTURE.md "Background bash
            // rows"). Only OnBackgroundTask's own terminal event ends its
            // liveness -- flipping `started` here would drop live deltas
            // (OnToolOutputDelta requires `started`) and read the row as
            // done while the task is still running.
            if (!it->is_background) it->started = false;   // no longer current
            return;
        }
    }
}

// ARCHITECTURE.md "Background bash rows". `tool_use_id` rides the SAME id as
// the ToolExecutionEvent that acked the launching (synchronous) call, so
// STARTED here typically finds a row OnToolStarted already opened -- it is
// promoted to a live background row rather than a second one being pushed.
// A row that OnToolFinished already closed, or a turn boundary already swept
// (before this event existed, or in the odd case the STARTED notice never
// arrived), REOPENS: "a client keyed by tool_use_id alone must tolerate a
// row reopening after its finish event" is the doc's own words for this.
void SessionState::OnBackgroundTask(const v1::BackgroundTaskEvent& evt) {
    std::lock_guard lk(mtx_);
    const bool is_terminal = evt.state() != v1::BackgroundTaskEvent::STARTED;
    const bool is_error = evt.state() == v1::BackgroundTaskEvent::FAILED ||
                          evt.state() == v1::BackgroundTaskEvent::TIMED_OUT ||
                          evt.state() == v1::BackgroundTaskEvent::KILLED;

    for (auto& tool : tools_) {
        if (tool.tool_use_id != evt.tool_use_id()) continue;
        tool.is_background = true;
        tool.started = !is_terminal;
        if (is_terminal) tool.is_error = is_error;
        if (!evt.command().empty()) tool.summary = evt.command();
        return;
    }

    // Reopen: no existing row (already finished-and-swept, or STARTED never
    // reached us before this terminal event did).
    ToolExecutionState tool;
    tool.tool_use_id = evt.tool_use_id();
    tool.tool_name = "bash";   // background tasks are bash's own extension
    tool.summary = evt.command();
    tool.is_background = true;
    tool.started = !is_terminal;
    tool.is_error = is_terminal && is_error;
    tools_.push_back(std::move(tool));
    // Same C4 marker OnToolStarted pushes -- a reopened background row (its
    // own STARTED never reached this client, or the row already finished
    // and was swept) is still live content for whatever turn is current now.
    live_segments_.push_back(LiveTurnSegment{.kind = LiveTurnSegment::Kind::Tool,
                                             .turn_seq = turn_seq_,
                                             .tool_use_id = evt.tool_use_id()});
}

// Scrollback -----------------------------------------------------------------

void SessionState::ApplyScrollbackPage(const v1::GetMessagesResponse& resp) {
    std::lock_guard lk(mtx_);
    refresh_succeeded_ = true;   // resolved OK, even when the page is empty
    scrollback_failures_ = 0;   // a success ends any failure streak (and its notice)
    if (resp.messages().empty()) {
        has_earlier_ = resp.has_more();   // no rows, but the server may still have older pages
        ++refresh_seq_;   // a resolved attempt (successful RPC, empty page) -- see NoteRefreshFailed
        return;
    }

    if (scrollback_page_.empty() ||
        resp.messages(0).seq() > scrollback_page_.front().seq()) {
        // Newer than what we hold — keep only the contiguous newest slice (bounded memory).
        scrollback_page_.assign(resp.messages().begin(), resp.messages().end());
        scrollback_trimmed_ = false;
        scrollback_gap_ = false;   // same reset as the newest-slice refresh's other boundary flag
    } else {
        // Older than (or overlapping) what we hold: append, SKIPPING rows we
        // already hold. The assign branch above must only ever fire when the
        // page's newest seq is genuinely newer than the window's -- a
        // duplicate in-flight "load older" fetch (two presses inside one RPC
        // round trip) returns the same before_seq page, and its apply taking
        // the assign branch REPLACED the window with the older page: the
        // newest rows vanished from the view.
        const int64_t oldest_held = scrollback_page_.back().seq();
        // Contiguity stitch: walking the page's DESC-order rows, each must be
        // exactly one below the previous seq we hold/accepted. A row that
        // lands BELOW that expectation means seqs are missing BETWEEN window
        // and page (or inside the page) -- a server-side hole (deleted rows,
        // a rewind race). The eviction marker (scrollback_trimmed_) cannot
        // describe a hole in the middle, so this records scrollback_gap_ for
        // the view the same way, and the newest-slice refresh clears both.
        // Rows already held (seq >= oldest_held, a raced duplicate) and rows
        // duplicating seqs the page itself already accepted are dropped.
        int64_t expected_prev = oldest_held - 1;
        for (const auto& stored : resp.messages()) {
            if (stored.seq() >= oldest_held) continue;   // already held (or a raced duplicate)
            if (stored.seq() > expected_prev) continue;  // duplicate within this page
            if (stored.seq() < expected_prev) scrollback_gap_ = true;
            expected_prev = stored.seq() - 1;
            scrollback_page_.push_back(stored);
        }
        // Sliding window (track R2): the wire has only desc-order paging
        // (before_seq), so there is no way to reload NEWER pages on demand --
        // eviction from the newest end is therefore flagged
        // (scrollback_trimmed_) and the view renders a boundary marker, never
        // a silent gap. The newest-slice refresh at turn end (below, and in
        // RequestScrollback(newest=true)) resets the flag and the window.
        while (static_cast<int>(scrollback_page_.size()) > scrollback_max_messages_) {
            scrollback_page_.erase(scrollback_page_.begin());
            scrollback_trimmed_ = true;
        }
    }

    last_loaded_seq_ = scrollback_page_.empty() ? -1 : scrollback_page_.back().seq();
    has_earlier_ = resp.has_more();
    // The one copy the shared page ever pays: rows changed, so the old
    // snapshot's view must stay stable for whatever frame is still reading it.
    scrollback_page_shared_ =
        std::make_shared<const std::vector<v1::StoredMessage>>(scrollback_page_);
    // The persisted user row (appended FIRST by the server, before any turn
    // output) is provably in this page once the page's NEWEST seq moved past
    // the submit-time baseline -- retire the optimistic echo and let the real
    // row (with the server-side @-mention expansion) take over. Newest, not
    // last_loaded_seq_: the window is DESCENDING, so back() is the
    // load-older cursor and moves when OLDER pages arrive too.
    if (!pending_user_text_.empty() && !scrollback_page_.empty() &&
        scrollback_page_.front().seq() > pending_user_baseline_seq_) {
        pending_user_text_.clear();
    }
    ++transcript_version_;
    ++refresh_seq_;
}

void SessionState::OnChatSubmitted(const std::string& text) {
    std::lock_guard lk(mtx_);
    pending_user_text_ = text;
    // The page's NEWEST seq (descending window: front, not back/back).
    pending_user_baseline_seq_ =
        scrollback_page_.empty() ? -1 : scrollback_page_.front().seq();
    // Immediate state feedback, too: the turn was ACCEPTED onto the wire, so
    // the tab is no longer Idle even before the server says anything.
    if (state_ == SessionStateKind::Idle) state_ = SessionStateKind::Working;
    // A stale /-command notice (an old /help listing, a stale error) used to
    // sit pinned across the WHOLE next turn -- persisted transcript rows are
    // the record of what happened, so a status line about the LAST command
    // has nothing to say about the new one the user just started.
    latest_notice_.reset();
    ++notice_seq_;
}

bool SessionState::CanLoadMore() const {
    std::lock_guard lk(mtx_);
    return has_earlier_;
}

void SessionState::NoteRefreshFailed(const std::string& reason) {
    std::lock_guard lk(mtx_);
    refresh_succeeded_ = false;   // resolved with a failure -- see Snapshot::refresh_succeeded
    ++refresh_seq_;   // resolved (no content change) -- lets a mismatch retry next frame
    // Retry PACING is scrollback_refresh.hpp's per-tab backoff; this counter
    // only decides when the silence ends. A server that refuses every page
    // (an undecodable transcript row answers INTERNAL) used to retry forever
    // with no user-visible symptom -- a transcript frozen mid-conversation.
    // Fire ONCE per streak (==, not >=): further failures extend the backoff
    // window but must not spam the notice line every retry.
    ++scrollback_failures_;
    if (scrollback_failures_ == kScrollbackFailureNoticeAfter) {
        latest_notice_ = CommandNotice{
            "transcript refresh failed " + std::to_string(scrollback_failures_) +
                " times in a row" + (reason.empty() ? "" : ": " + reason) +
                "; still retrying -- the transcript shown may be stale",
            /*is_error=*/true, v1::COMMAND_DISPLAY_STATUS, /*view_hint=*/{}};
        ++notice_seq_;
    }
}

bool SessionState::TryBeginOlderFetch() {
    std::lock_guard lk(mtx_);
    if (older_fetch_pending_) return false;
    older_fetch_pending_ = true;
    return true;
}

void SessionState::EndOlderFetch() {
    std::lock_guard lk(mtx_);
    older_fetch_pending_ = false;
}

// ===========================================================================
// AppState — manages all tabs (roots + subagent children), focus, derived UI.
// Each public method locks mtx_ for its container mutation so a single app-level view
// is consistent; per-tab SessionState owns its own mutex.
// ===========================================================================

namespace {

// Which non-OK stream statuses mean "the server is unreachable" versus "the
// server deliberately ended this RPC". The first probe run settled it: a
// server-side session termination surfaces as NOT_FOUND ("unknown session_id")
// -- an application response from a live server, which must reach Done, not a
// "connection lost" error. A crashed server fails the call with UNAVAILABLE /
// INTERNAL / DEADLINE_EXCEEDED.
bool IsTransportDeath(const grpc::Status& status) {
    switch (status.error_code()) {
        case grpc::StatusCode::UNAVAILABLE:
        case grpc::StatusCode::INTERNAL:
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            return true;
        default:
            return false;
    }
}

// The attach-time refusals OpenSession can answer with INSTEAD of attaching
// (unknown session_id, session already terminated, subagent resumed
// standalone, resume validation): the bidi never carried a live session, and
// the tab must say WHY it is dead rather than render the clean "Done ✓" of a
// session that actually ran. Code alone cannot always carry that distinction
// -- a stream refused at attach and a stream that ran and then lost its
// session can end with the same NOT_FOUND -- so callers pair this with
// Stream::received_any() (see WireOnClosed).
bool IsAttachRefusal(const grpc::Status& status) {
    switch (status.error_code()) {
        case grpc::StatusCode::NOT_FOUND:
        case grpc::StatusCode::ALREADY_EXISTS:
        case grpc::StatusCode::FAILED_PRECONDITION:
        case grpc::StatusCode::INVALID_ARGUMENT:
            return true;
        default:
            return false;
    }
}

// Human names for the ErrorEvent taxonomy (common.proto LlmErrorCategory) --
// what the error notice renders in parentheses next to the provider's message.
// nullptr for UNSPECIFIED: an unnamed category contributes nothing.
const char* ErrorCategoryName(v1::LlmErrorCategory category) {
    switch (category) {
        case v1::LLM_ERROR_CATEGORY_TRANSPORT: return "transport";
        case v1::LLM_ERROR_CATEGORY_RATE_LIMIT: return "rate limit";
        case v1::LLM_ERROR_CATEGORY_SERVER_ERROR: return "server error";
        case v1::LLM_ERROR_CATEGORY_AUTH_ERROR: return "auth";
        case v1::LLM_ERROR_CATEGORY_REQUEST_ERROR: return "request";
        case v1::LLM_ERROR_CATEGORY_PARSE_ERROR: return "parse";
        default: return nullptr;
    }
}

}  // namespace

AppState::AppState(std::shared_ptr<SessionClient> client) : client_(std::move(client)) {}

// The whole point of ~AppState running CloseAllTabs: leave NO stream alive
// when member destruction begins. Every tab's reader thread is joined inside
// stream->Close() (its on_closed_ callback still sees a fully-alive AppState
// -- tabs_ cleared but valid), so by the time tabs_'s destructor runs no
// callback can reach into this object. Detach would be wrong here: it leaves
// the client threads running, exactly the half-destroyed-race this prevents.
AppState::~AppState() { CloseAllTabs(); }

void AppState::SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes) {
    if (scrollback_max_messages > 0) scrollback_max_messages_ = scrollback_max_messages;
    if (tool_output_buffer_bytes > 0) tool_output_buffer_bytes_ = tool_output_buffer_bytes;
}

void AppState::DisableChatSurface(const char* reason) {
    chat_surface_broken_.store(true, std::memory_order_release);
    if (reason != nullptr) {
        common::DefaultLogger().error(
            "chat surface plugin threw ({}) -- disabled; the core chat view takes over", reason);
    } else {
        common::DefaultLogger().error(
            "chat surface plugin threw a non-standard exception -- disabled; the core chat "
            "view takes over");
    }
}

void AppState::ApplyUiBounds(SessionState& state) const {
    state.SetBounds(
        scrollback_max_messages_ > 0 ? scrollback_max_messages_
                                     : SessionState::kDefaultScrollbackWindowMaxMessages,
        tool_output_buffer_bytes_ > 0 ? tool_output_buffer_bytes_
                                      : SessionState::kDefaultToolOutputBufferBytes);
}

void AppState::WireOnClosed(SessionClient::Stream& stream, const std::string& session_id) {
    stream.SetOnClosed([this, session_id, &stream](const grpc::Status& status) {
        if (auto state = GetState(session_id)) {
            // The reader's Finish() status distinguishes transport death, an
            // attach refusal, and a clean server-side close: a crashed server
            // used to render as a happy "Done ✓" tab that silently swallowed
            // typed input, and so did an attach the server refused (the
            // picker-resume bug: the dead tab even RENDERED its history,
            // because scrollback reads the database directly). The status
            // RIDES IN on the callback -- reaching back into the stream from
            // here could make this callback the stream's last owner while the
            // reader thread is inside it (see Stream::SetOnClosed) -- the one
            // safe read is `received_any()`, an atomic the reader itself set.
            //
            // The refusal check NEEDS that flag: a terminated session's stream
            // ends with the very same NOT_FOUND an attach refusal carries
            // ("unknown session_id" / "already terminated"), and it must reach
            // the clean Done. Only a stream that never delivered a single
            // event -- attach refused -- is an error.
            if (IsTransportDeath(status)) {
                state->OnError("connection lost: " + status.error_message());
            } else if (!stream.received_any() && IsAttachRefusal(status)) {
                state->OnError("session attach failed: " + status.error_message());
            } else {
                state->OnSessionEnded();
            }
            BumpUiVersion();   // a close with no preceding event must still repaint
        }
    });
}

// Open a brand-new tab. Sessionless by construction: nothing is created
// server-side and no stream exists until the first message forces one
// (SubmitChatTurn -> EnsureSession). `opts` are parked as the tab's pending
// create-options -- including any /model //agent choices the user stages
// before typing -- and go out verbatim with that eventual CreateSession.
// Returns immediately; this is the property that makes "+ new session", /new
// and the empty-app command flows incapable of leaving a 0-message row behind.
std::string AppState::OpenTab(const SessionClient::CreateOptions& opts) {
    std::string tab_id;
    {
        std::lock_guard lk(mtx_);
        TabEntry entry;
        entry.info.state = std::make_shared<SessionState>(/*session_id=*/"");
        ApplyUiBounds(*entry.info.state);
        entry.info.tab_id = "t" + std::to_string(++next_tab_seq_);
        tab_id = entry.info.tab_id;
        entry.pending_create = opts;
        entry.info.parent_session_id = std::nullopt;   // root tab (a child gets its parent set)
        entry.info.root_ordinal = ++next_root_ordinal_;   // a fresh group id
        entry.info.spawn_seq = next_spawn_seq_++;

        if (focused_id_.empty()) focused_id_ = tab_id;   // focus an empty app on its first tab
        tabs_.push_back(std::move(entry));
    }
    BumpUiVersion();
    return tab_id;
}

// Attach an existing server-side session as a tab. Same two-phase pattern: name+register,
// then Resume() so its child's events find this entry rather than racing it.
std::string AppState::AttachTab(const std::string& session_id, int64_t parent_ordinal) {
    std::string registered;   // the new tab's tab_id, set once registration commits
    // Duplicate refusal BEFORE any wire traffic: attaching an already-open
    // session would create a second TabEntry whose events all route to the
    // first (FindTab returns the first match), leaving a permanently dead
    // tab. Picker resumes of an open session are the realistic path.
    {
        std::lock_guard lk(mtx_);
        if (FindTabBySession(session_id) != nullptr) return "";
    }

    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });

    const auto attached = stream->Attach(session_id);   // names the stream (threads off)
    if (!attached) return "";

    bool duplicate = false;
    {   // register under a lock, then drop it before starting the reader thread
        std::lock_guard lk(mtx_);
        if (FindTabBySession(session_id) != nullptr) {
            // A duplicate slipped through the race window (two concurrent
            // AttachTab calls -- requires the server to double-emit a
            // SubagentStartedEvent). Cancel the underlying call and drop it:
            // its naming event was never sent, so the server never attached
            // it and cancelling cannot terminate the real session the way
            // Finish()-without-cancel could wedge or a close could destroy.
            duplicate = true;
        } else {
            TabEntry entry;
            entry.stream = stream;
            entry.info.state = std::make_shared<SessionState>(session_id);
            ApplyUiBounds(*entry.info.state);
            entry.info.tab_id = "t" + std::to_string(++next_tab_seq_);
            entry.info.session_id = session_id;

            // Parent-of-group: locate the leader (root) of this group so subagents inherit its ordinal.
            if (parent_ordinal >= 0) {
                for (const auto& tab : tabs_) {
                    if (tab.info.root_ordinal == parent_ordinal && !tab.info.parent_session_id.has_value()) {
                        entry.info.parent_session_id = tab.info.session_id;   // immediate parent
                        entry.info.root_ordinal = parent_ordinal;             // inherit group id
                        break;
                    }
                }
            }
            if (!entry.info.parent_session_id.has_value()) {
                entry.info.parent_session_id = std::nullopt;          // standalone root when no leader found
                entry.info.root_ordinal = ++next_root_ordinal_;
            }

            entry.info.spawn_seq = next_spawn_seq_++;
            // Focus an empty app on its first tab (OpenTab's rule): a picker
            // resume into a fresh session-less TUI must show the session, not
            // the welcome placeholder.
            if (focused_id_.empty()) focused_id_ = entry.info.tab_id;
            registered = entry.info.tab_id;
            tabs_.push_back(std::move(entry));
        }
    }   // lock released here, before the reader thread can dispatch back into us

    if (duplicate) {
        // Cancel and drop OUTSIDE mtx_: the stream's destructor runs Close(),
        // whose Finish() plucks the gRPC completion queue -- a blocking call.
        // Under mtx_ this is the one way an app-state lock holder blocks
        // forever (a wedged call never unwinds), parking every sender and
        // every reader callback behind it.
        stream->Cancel();
        return "";
    }

    WireOnClosed(*stream, session_id);
    stream->Resume();   // start threads AFTER unlock — same guard as OpenTab
    return registered;
}

// Resume a STORED session (the picker's "open a past session" action): the
// CreateSession RPC is re-issued with resume_session_id so the server revives
// the session from its transcript (ResumeSession) BEFORE the bidi names it.
// Bare attach (AttachTab) resolves its id against the LIVE registry only, so
// it answered NOT_FOUND for every session from a previous run -- and the dead
// stream then swallowed every prompt and /command silently while the tab still
// rendered its history (scrollback reads the database directly). See
// Stream::Open's resume branch for the wire shape.
std::string AppState::ResumeTab(const std::string& session_id) {
    std::string existing_tab_id;   // set when the session is already open here
    {   // already open here: focusing it (the picker's next call) beats a
        // duplicate stream against one live session
        std::lock_guard lk(mtx_);
        if (const TabEntry* found = FindTabBySession(session_id);
            found != nullptr) {
            existing_tab_id = found->info.tab_id;
        }
    }
    if (!existing_tab_id.empty()) return existing_tab_id;

    // Resume carries session_id (and the client-owned run_mode) ONLY: the
    // stored row's name/preset/project_root govern, and the server rejects
    // the rest (SessionServiceImpl::ResumeSession).
    SessionClient::CreateOptions opts;
    opts.resume_session_id = session_id;

    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });
    const auto opened = stream->Open(opts);
    if (!opened) {
        // The resume was refused and NOTHING was attached (Open names the bidi
        // only on success): the stream object is inert and simply dropped. The
        // refusal is not logged-and-forgotten either -- the tab below carries
        // it visibly.
        common::DefaultLogger().error("resume of session {} failed: {}", session_id,
                                      stream->status().error_message());
    }
    const bool live = opened.has_value();
    std::string registered;   // the new tab's tab_id, set once registration commits

    bool duplicate = false;
    {   // register under a lock, then drop it before starting the reader thread
        std::lock_guard lk(mtx_);
        if (FindTabBySession(session_id) != nullptr) {
            // A duplicate slipped through the race window (two concurrent
            // ResumeTab calls): same rule as AttachTab's -- the naming event
            // was never sent (threads off), so cancelling the call cannot
            // disturb the session the winning caller is resuming.
            duplicate = true;
            if (const TabEntry* found = FindTabBySession(session_id);
                found != nullptr) {
                registered = found->info.tab_id;
            }
        } else {
            TabEntry entry;
            entry.stream = live ? stream : nullptr;   // copy: `stream` is used below
            entry.info.state = std::make_shared<SessionState>(session_id);
            ApplyUiBounds(*entry.info.state);
            entry.info.tab_id = "t" + std::to_string(++next_tab_seq_);
            entry.info.session_id = session_id;
            entry.info.parent_session_id = std::nullopt;   // a resumed past session is its own root
            entry.info.root_ordinal = ++next_root_ordinal_;
            entry.info.spawn_seq = next_spawn_seq_++;
            // Focus an empty app on its first tab (OpenTab's rule).
            if (focused_id_.empty()) focused_id_ = entry.info.tab_id;
            if (!live) {
                entry.info.state->OnError("resume failed: " + stream->status().error_message());
            }
            registered = entry.info.tab_id;
            tabs_.push_back(std::move(entry));
        }
    }   // lock released here, before the reader thread can dispatch back into us

    if (duplicate) {
        // Cancel OUTSIDE mtx_ (see AttachTab): the stream destructor's
        // Finish() plucks the completion queue and must never run under the
        // app-state lock.
        stream->Cancel();
        return registered;
    }
    BumpUiVersion();

    if (live) {
        WireOnClosed(*stream, session_id);
        stream->Resume();   // start threads AFTER unlock -- same guard as OpenTab/AttachTab
    }
    return registered;
}

bool AppState::HasTab(const std::string& tab_id) const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == tab_id) return true;
    }
    return false;
}

// Reader-thread callback: route a ServerEvent to its owned tab. Defers subagent creation
// until the app lock is released, so opening a child stream never happens while held.
void AppState::DispatchToTab(const std::string& session_id, const v1::ServerEvent& event) {
    BumpUiVersion();   // every wire event can change what the frame renders
    // The exclusive chat surface sees EVERY event for the session, before
    // core's handling (docs/EXTENSIBILITY.md 4.2: the host pushes, the surface
    // owns its presentation; core's data plane runs regardless). Reader
    // thread -- OnEvent must not block; that is the surface's contract.
    // Throwing is contained: the surface is disabled for the process (the
    // core chat view takes over). An UNguarded throw here propagated out of
    // the reader thread function and std::terminate'd the whole TUI -- and a
    // blocking surface wedges the data plane AND the CloseAllTabs join; the
    // block contract is documented, the throw is enforced.
    if (chat_surface_ != nullptr && !chat_surface_broken_.load(std::memory_order_acquire)) {
        try {
            chat_surface_->OnEvent(event);
        } catch (const std::exception& error) {
            chat_surface_broken_.store(true, std::memory_order_release);
            common::DefaultLogger().error(
                "chat surface plugin threw ({}) -- disabled; the core chat view takes over",
                error.what());
        } catch (...) {
            chat_surface_broken_.store(true, std::memory_order_release);
            common::DefaultLogger().error(
                "chat surface plugin threw a non-standard exception -- disabled; the core "
                "chat view takes over");
        }
    }

    // Owning copy out of the lookup: CloseTab can erase the TabEntry (dropping
    // the last shared_ptr) while this reader-thread dispatch is in flight — a
    // raw pointer here was a use-after-free on every tab close with a live
    // stream.
    const auto target = GetState(session_id);
    if (!target) return;   // orphaned / unknown id (e.g. pre-attach race) — ignore

    // Use has_* predicates (not the oneof case enum) so this stays correct across protobuf
    // codegen versions; each ServerEvent carries exactly one of these fields.
    if (event.has_turn_completed()) {
        target->OnTurnCompleted(event.turn_completed());   // Done + occupancy metrics
    } else if (event.has_error()) {
        // The ErrorEvent taxonomy fields (category/http_status/is_retryable)
        // are produced server-side; rendering them into the visible text is
        // what makes a non-retryable failure READ as one instead of a generic
        // error the user keeps re-sending.
        const auto& err = event.error();
        std::string message = err.message();
        std::string tag;
        if (const char* category = ErrorCategoryName(err.category())) tag = category;
        if (err.has_http_status()) {
            if (!tag.empty()) tag += ", ";
            tag += "HTTP " + std::to_string(err.http_status());
        }
        if (!tag.empty()) message += " (" + tag + ")";
        if (!err.is_retryable()) message += " [non-retryable]";
        target->OnError(message);
    } else if (event.has_resync_required()) {
        // S5: this stream's delivery had a hole. The durable record is the
        // transcript; bumping the version makes the next maintain() pass
        // refetch the newest scrollback through the async RPC queue (T2).
        target->OnResyncRequired(event.resync_required().reason());
    } else if (event.has_prompt_resolved()) {
        // S5.3: another subscriber answered first. Dismiss this tab's modal
        // for that request_id if it still has one open -- a no-op on the tab
        // that answered, since it already cleared its own on send.
        // OnPendingResolved must ride the same "actually cleared" condition:
        // the dismissed prompt was the only thing parking the tab (the
        // server worker was blocked on it), so the tab needs the same
        // re-derive the local answer path does (AnswerConfirmation) --
        // without it a tab whose prompt was answered elsewhere stranded in
        // Waiting (the '!' glyph) and a command confirmation never recovered.
        if (target->ClearPendingFor(event.prompt_resolved().request_id())) {
            target->OnPendingResolved();
        }
    } else if (event.has_session_metadata()) {
        target->OnSessionMetadata(event.session_metadata());   // refreshes tab label later
    } else if (event.has_session_ended()) {
        // THE TERMINAL EVENT (Session::Terminate publishes it before closing
        // every subscriber queue): a session terminated server-side while this
        // client was attached. Unrouted, it went silent -- the stream itself
        // stays open (the server's read loop keeps reading), so the tab could
        // only leave Done-marking to a stream close that never comes, and
        // every send into the dead session vanished server-side. Terminal
        // state, same as a stream close: pending modals are dropped (nothing
        // can answer them anymore).
        target->OnSessionEnded();
    } else if (event.has_session_rewound()) {
        // Rewind/redo (proto: SessionRewoundEvent): trim the scrollback to the
        // ACTIVE transcript and clear streaming/pending-turn state; the version
        // bump inside makes the refresh state machine refetch what survived.
        target->OnSessionRewound(event.session_rewound().truncated_to_seq(),
                                 event.session_rewound().active_last_seq());
    } else if (event.has_stream_event()) {
        const auto& se = event.stream_event();
        if (se.has_text_delta()) {
            target->AppendStreamingText(se.text_delta().text());
        } else if (se.has_thinking_delta()) {
            // Same live-path obligation as text_delta: dropping these made
            // thinking appear only in the turn-end refetch (a visible jump).
            target->AppendStreamingThinking(se.thinking_delta().text());
        } else if (se.has_usage()) {
            target->OnUsage(se.usage());
        } else if (se.has_content_block_stop() || se.has_message_stop()) {
            target->FinalizeStreamingText();
        }
        // message_start carries only the provider's message id/model, which
        // the TUI derives from SessionMetadataEvent instead -- nothing to
        // accumulate, deliberately unhandled.
    } else if (event.has_permission_request()) {
        const auto& pr = event.permission_request();
        auto pp = std::make_unique<PendingPermission>();
        pp->request_id = pr.request_id();
        pp->tool_name = pr.tool_name();
        pp->category = pr.category();
        pp->summary = pr.summary();
        pp->complete = pr.complete();
        pp->grants.assign(pr.grants().begin(), pr.grants().end());
        target->SetPermissionRequest(std::move(pp));
    } else if (event.has_confirmation_request()) {
        const auto& cr = event.confirmation_request();
        auto cp = std::make_unique<PendingConfirmation>();
        cp->request_id = cr.request_id();
        cp->kind = cr.kind();
        cp->title = cr.title();
        cp->detail = cr.detail();
        cp->options.assign(cr.options().begin(), cr.options().end());
        target->SetConfirmationRequest(std::move(cp));
    } else if (event.has_tool_execution()) {
        const auto& te = event.tool_execution();
            if (te.started()) {
                target->OnToolStarted(te);
            } else {
                target->OnToolFinished(te.tool_use_id(), te);   // finished tool
            }
    } else if (event.has_tool_output_delta()) {
        target->OnToolOutputDelta(event.tool_output_delta().tool_use_id(),
                                   event.tool_output_delta().chunk());
    } else if (event.has_background_task()) {
        target->OnBackgroundTask(event.background_task());
    } else if (event.has_command_result()) {
        target->OnCommandResult(event.command_result());
    } else if (event.has_subagent_started()) {
        std::lock_guard lk(mtx_);   // append only; deferred spawn happens after unlock below
        pending_subagents_.emplace_back(session_id, event.subagent_started());
        has_pending_subagents_.store(true, std::memory_order_release);
    } else {
        // No final else used to exist: an UNSET payload and an UNKNOWN future
        // case (a NEWER server against this client) both fell through
        // silently. Warn at least, so wire evolution leaves a trace.
        minimal::common::DefaultLogger().warn(
            "session {}: server event with {} payload (case {})",
            session_id, event.payload_case() == v1::ServerEvent::PAYLOAD_NOT_SET
                            ? "no"
                            : "unknown",
            static_cast<int>(event.payload_case()));
    }
    // command_result is stored as the tab's transient notice above (routed by
    // its display mode in RunTui); PluginCall data is a plugin's own path.

    // The app lock is released now. Spawn any deferred subagent tabs so child streams attach
    // on their own reader threads without ever holding mtx_ while opening (which could deadlock).
    // Gated on the atomic (see its declaration): subagent spawns are rare
    // relative to the stream/tool events that call DispatchToTab, and taking
    // mtx_ a second time on every one of them just to swap out an empty
    // vector was pure waste. A thread that reads `false` here needs no lock
    // to know pending_subagents_ is empty -- that is exactly what the
    // release/acquire pairing on the flag guarantees.
    if (has_pending_subagents_.load(std::memory_order_acquire)) {
        std::vector<std::pair<std::string, v1::SubagentStartedEvent>> spawned;
        {
            std::lock_guard lk(mtx_);
            std::swap(pending_subagents_, spawned);   // drain under a fresh lock
            has_pending_subagents_.store(false, std::memory_order_release);
        }
        for (auto& [parent_id, subevt] : spawned) {
            OnSubagentSpawned(parent_id, subevt);
        }
    }
}

void AppState::OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt) {
    // Teardown race: a SubagentStartedEvent can land on a reader thread while
    // quit is running (DetachAllTabs/CloseAllTabs). Attaching then would open
    // a stream AFTER the tabs_ snapshot -- teardown never sees it, never
    // detaches it, and the tab's SessionState is touched mid-destruction.
    // Drop the spawn instead: the session keeps running server-side and is
    // resumable, exactly like every other session outlived by a quit.
    if (shutting_down_.load(std::memory_order_acquire)) return;
    int64_t parent_root = 0;
    {   // find the group id of the parent BEFORE attaching so the child inherits it stably
        std::lock_guard lk(mtx_);
        for (const auto& tab : tabs_) {
            if (tab.info.session_id == parent_id) {
                parent_root = tab.info.root_ordinal;
                break;
            }
        }
    }
    const std::string child_tab = AttachTab(evt.child_session_id(), parent_root);

    // Subagent rows are created nameless server-side (subagent_manager.cpp
    // mints the row with name=""), so the child tab's label used to fall back
    // to the raw session id until -- if ever -- a naming observer ran. The
    // event's `task` (the parent model's own formulation of the job) is the
    // natural title; seed it ONLY while the tab is still unnamed and idle, so
    // a real SessionMetadataEvent name always wins and a racing live turn is
    // never disturbed.
    if (!evt.task().empty()) {
        if (auto child_state = GetState(evt.child_session_id())) {
            const auto snap = child_state->TakeLightSnapshot();
            if (snap.display_name.empty() && snap.state == SessionStateKind::Idle) {
                child_state->OnSessionCreated(evt.task());
            }
        }
    }

    // A blocking subagent pins its parent's turn, so focus follows it (parent is stuck waiting).
    if (evt.blocking() && !child_tab.empty()) SetFocusedTab(child_tab);
}

void AppState::CloseTab(const std::string& tab_id) {
    BumpUiVersion();
    std::shared_ptr<SessionClient::Stream> to_close;
    bool was_focused = false;
    {
        std::lock_guard lk(mtx_);
        for (auto it = tabs_.begin(); it != tabs_.end(); ++it) {
            if (it->info.tab_id == tab_id) {
                to_close = it->stream;
                was_focused = (focused_id_ == tab_id);
                // erase() returns an iterator to whatever tab shifted into
                // the closed one's position -- the NEXT tab in render
                // order, if any. Focusing that (falling back to the new
                // last tab when the closed one WAS the last, i.e. `next`
                // came back as end()) keeps focus on a NEIGHBOR of the tab
                // that closed. Unconditionally jumping to tabs_.back()
                // instead sent focus to the far end of the tab bar
                // regardless of which tab closed -- closing tab 2 of 10
                // landed on tab 10.
                const auto next = tabs_.erase(it);
                if (was_focused) {
                    // ... and when the LAST tab closed there is no neighbor:
                    // focus must clear (as CloseAllTabs/DetachAllTabs do), or
                    // FocusedTabId() dangles on the closed id -- reads as
                    // "a tab is focused" to every caller, and the next
                    // picker-resume's focus-if-empty rule never fires.
                    focused_id_ = !tabs_.empty()
                                      ? (next != tabs_.end() ? next : std::prev(tabs_.end()))
                                            ->info.tab_id
                                      : std::string{};
                }
                break;
            }
        }
    }
    if (to_close) to_close->Close();   // drains + joins; terminates the session when last open
}

void AppState::SetFocusedTab(const std::string& tab_id) {
    BumpUiVersion();
    std::lock_guard lk(mtx_);
    // FindTab, not HasTab: HasTab locks mtx_ itself, and this mutex is not
    // recursive -- calling it here would deadlock every caller, including
    // FocusWithWrap and the blocking-subagent auto-focus path.
    if (FindTab(tab_id) != nullptr) focused_id_ = tab_id;
}

void AppState::CycleFocusForward() { FocusWithWrap(true); }
void AppState::CycleFocusBackward() { FocusWithWrap(false); }

void AppState::FocusWithWrap(bool forward) {
    std::vector<std::string> order;
    // focused_id_ copied under the SAME lock as order: a reader thread can
    // write it via OnSubagentSpawned -> SetFocusedTab (the blocking-subagent
    // auto-focus path) at any time, so reading it after the lock was
    // released -- as this used to -- was an unlocked read racing that write
    // (TSan-visible; no crash, just an occasionally stale `current`).
    std::string focused;
    {
        std::lock_guard lk(mtx_);
        for (const auto& tab : tabs_) order.push_back(tab.info.tab_id);
        focused = focused_id_;
    }
    if (order.empty()) return;

    const auto current = std::find(order.begin(), order.end(), focused);
    if (current == order.end()) {     // nothing focused yet — focus first in render order
        SetFocusedTab(order.front());
        return;
    }

    const ssize_t idx = static_cast<ssize_t>(current - order.begin());
    const ssize_t total = static_cast<ssize_t>(order.size());
    const ssize_t next = forward ? (idx + 1) % total
                                 : ((idx - 1 + total) % total);
    SetFocusedTab(order[next]);
}

void AppState::SendChatTurn(const std::string& session_id, const std::string& text) {
    auto stream = GetStream(session_id);
    if (!stream) {
        // A tab with no stream (a refused resume) must say so: a silent
        // return here reads as "the model ignored me".
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: this session is not attached");
            BumpUiVersion();
        }
        return;
    }
    if (RefuseIfEnded(session_id, "send")) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    ev.mutable_chat_turn()->set_text(text);
    // A refused send is surfaced, not swallowed: the send queue is bounded
    // (the server stopped reading) or the stream is dead -- either way, typed
    // input silently vanishing reads as "the model ignored me".
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: the stream is closing or stalled");
            BumpUiVersion();
        }
        return;
    }

    // OPTIMISTIC ECHO. The persisted user row only reaches a page through a
    // fetch, and nothing fetched mid-turn -- so the submitted message did not
    // render until the turn ENDED, after the assistant's streaming reply had
    // been visible for the whole turn (the reported "assistant renders before
    // user hi"). Echo the text now; the server-side @-mention expansion
    // (attachments render differently from raw text) arrives with the
    // persisted row and retires the echo.
    if (auto state = GetState(session_id)) {
        state->OnChatSubmitted(text);
        BumpUiVersion();
    }
    // And fetch eagerly: with the echo covering the latency, this is what
    // swaps in the authoritative expanded row within one round trip instead
    // of at turn end. One redundant fetch when the turn-end refresh fires
    // anyway; the refresh machine tolerates out-of-band applies.
    RequestScrollback(session_id, /*newest=*/true);
}

// --- Sessionless-tab submission layer ---------------------------------------
//
// The only paths that can create a session. On a sessioned tab both delegate
// inline (the loop thread's fire-and-forget enqueue, exactly as before); on a
// sessionless tab the action is parked on the tab's pending queue and
// EnsureSession runs once on the RPC worker -- so a message typed and a
// /-command sent before the create resolves land on ONE session, and an
// abandoned sessionless tab never reaches the server at all.

void AppState::SubmitChatTurn(const std::string& tab_id, const std::string& text) {
    // The sessioned fast path MUST call SendChatTurn OUTSIDE the lock: the
    // senders take mtx_ themselves (GetStream/GetState), and this mutex is
    // not recursive.
    std::string sessioned;
    {
        std::lock_guard lk(mtx_);
        if (TabEntry* tab = FindTab(tab_id); tab != nullptr) {
            if (tab->stream != nullptr) {
                sessioned = tab->info.session_id;
            } else {
                tab->pending.push_back([this, text](const std::string& sid) {
                    SendChatTurn(sid, text);
                });
            }
        }
    }
    if (!sessioned.empty()) {
        SendChatTurn(sessioned, text);
        return;
    }
    RunEnsure(tab_id);
}

void AppState::SubmitCommand(const std::string& tab_id, const std::string& line) {
    std::string sessioned;
    {
        std::lock_guard lk(mtx_);
        if (TabEntry* tab = FindTab(tab_id); tab != nullptr) {
            if (tab->stream != nullptr) {
                sessioned = tab->info.session_id;
            } else {
                tab->pending.push_back([this, line](const std::string& sid) {
                    SendCommand(sid, line);
                });
            }
        }
    }
    if (!sessioned.empty()) {
        SendCommand(sessioned, line);
        return;
    }
    RunEnsure(tab_id);
}

// Kicks EnsureSession onto the RPC worker. Public entry points call this
// without holding mtx_ (RunOnRpcQueue takes its own internal lock).
void AppState::RunEnsure(const std::string& tab_id) {
    RunOnRpcQueue([this, tab_id] { EnsureSession(tab_id); });
}

void AppState::OnTabEnsureSession(const std::string& tab_id,
                                  std::function<void(const std::string& session_id)> action) {
    // Like SubmitChatTurn: the sessioned inline run must happen OUTSIDE the
    // lock (actions may re-enter AppState's own locked methods).
    std::string sessioned;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) return;
        if (tab->stream != nullptr) {
            sessioned = tab->info.session_id;
        } else {
            tab->pending.push_back(std::move(action));
        }
    }
    if (!sessioned.empty()) {
        action(sessioned);   // sessioned: inline, no queue hop
        return;
    }
    RunEnsure(tab_id);
}

// RPC-worker ONLY (RunEnsure schedules it there): the blocking CreateSession
// round trip must never run on the loop thread. Single-flight per app -- the
// RPC queue has one worker, so a second kick while one create is in flight
// queues behind it and finds the tab already sessioned.
std::string AppState::EnsureSession(const std::string& tab_id) {
    SessionClient::CreateOptions opts;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) return "";   // closed while queued
        if (tab->stream != nullptr) return tab->info.session_id;   // ensure raced a winner
        if (tab->ensuring) return "";   // this call is queued behind the in-flight one
        tab->ensuring = true;
        opts = tab->pending_create;
    }

    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });
    const auto sid = stream->Open(opts);   // creates session, names bidi (threads off)

    bool tab_gone = false;      // closed while the create was in flight
    bool create_failed = false; // the create was refused (or no server reachable)
    std::vector<std::function<void(const std::string&)>> drained;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) {
            // Closed while the create was in flight: the stream is inert
            // (its naming event was sent but Resume() never started any
            // thread to read or write more) -- cancelling ends the call
            // without touching the session the server just minted beyond
            // what the reaper's never-attached guard already collects.
            tab_gone = true;
        } else {
            tab->ensuring = false;
            if (!sid) {
                // The create was refused (or no server reachable). The tab
                // STAYS -- a sessionless tab is a real tab -- with the
                // refusal visible, and everything queued behind the create
                // is dropped: their senders see this notice, the same
                // visible outcome a failed synchronous create always had.
                tab->info.state->OnError("could not start a session: " +
                                         stream->status().error_message());
                create_failed = true;
            } else {
                tab->stream = stream;
                tab->info.session_id = *sid;
                tab->info.state->SetSessionId(*sid);
                drained.swap(tab->pending);
            }
        }
    }
    BumpUiVersion();

    if (tab_gone) {
        // Cancel OUTSIDE mtx_ (see AttachTab): the stream destructor's
        // Finish() plucks the completion queue and must never run under the
        // app-state lock.
        stream->Cancel();
        return "";
    }
    if (create_failed) return "";

    WireOnClosed(*stream, *sid);
    stream->Resume();   // start threads AFTER unlock — a dispatched event must find this entry
    for (auto& action : drained) action(*sid);
    return *sid;
}

bool AppState::StageModel(const std::string& tab_id, const std::string& model_ref) {
    std::lock_guard lk(mtx_);
    TabEntry* tab = FindTab(tab_id);
    if (tab == nullptr || tab->stream != nullptr) return false;
    tab->pending_create.model = model_ref;
    tab->info.state->StageDisplayModel(model_ref);
    BumpUiVersion();
    return true;
}

bool AppState::StageAgentPreset(const std::string& tab_id, const std::string& preset_name) {
    std::lock_guard lk(mtx_);
    TabEntry* tab = FindTab(tab_id);
    if (tab == nullptr || tab->stream != nullptr) return false;
    tab->pending_create.agent_preset = preset_name;
    tab->info.state->StageDisplayAgentPreset(preset_name);
    BumpUiVersion();
    return true;
}

void AppState::RunSessionlessCommand(const std::string& tab_id, const std::string& line) {
    const auto state = GetStateByTab(tab_id);
    if (!state) return;
    const auto invocation = SplitCommand(line);
    // The RPC runs on the async worker (bounded deadline -- see
    // SessionClient::RunSessionlessCommand); the result publishes into the
    // tab's SessionState exactly like an on-stream CommandResult, so the
    // notice line and the View side-effects (TabMaintenance::HandleNotice)
    // treat both identically.
    // must_run: the command result resolves a user action (the tab's notice
    // line / view side-effects); a keystroke-class suggestion may be dropped,
    // an answered command may not (rpc_queue.hpp).
    rpc_queue_.Submit([this, state, invocation] {
        const auto result = client_->RunSessionlessCommand(invocation.name, invocation.positional,
                                                           invocation.raw);
        state->OnCommandResult(result);
        BumpUiVersion();
    }, /*must_run=*/true);
}

void AppState::TabNotice(const std::string& tab_id, const std::string& message) {
    if (auto state = GetStateByTab(tab_id)) {
        v1::CommandResult notice;
        notice.set_status(v1::COMMAND_STATUS_OK);
        notice.set_message(message);
        state->OnCommandResult(notice);
        BumpUiVersion();
    }
}

void AppState::RequestSuggestPaths(const std::string& session_id, const std::string& prefix,
                                   int32_t limit,
                                   std::function<void(std::vector<std::string>)> on_result) {
    rpc_queue_.Submit([this, session_id, prefix, limit, on_result = std::move(on_result)] {
        const auto result = client_->SuggestPaths(session_id, prefix, limit);
        std::vector<std::string> paths;
        if (result.status.ok()) {
            paths.assign(result.response.paths().begin(), result.response.paths().end());
        }
        if (on_result) on_result(std::move(paths));
        BumpUiVersion();   // wake the pump so the editor's next poll sees it
    });
}

void AppState::AnswerPermission(const std::string& session_id, const std::string& request_id,
                                v1::PermissionDecision decision,
                                std::vector<std::string> granted_keys) {
    BumpUiVersion();
    // Mirror the wire guarantee (see ARCHITECTURE.md consent scoping): only keys actually
    // offered by the PermissionRequest may be answered. The server filters extras too, but
    // answering is a local no-op here so the round-trip stays cheap.
    auto stream = GetStream(session_id);
    if (!stream) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot answer: this session is not attached");
        }
        return;
    }
    if (RefuseIfEnded(session_id, "answer")) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* pr = ev.mutable_permission_response();
    pr->set_request_id(request_id);
    pr->set_decision(decision);
    for (auto& k : granted_keys) pr->add_granted_keys(k);
    // A refused send must NOT close the prompt: the server worker is parked
    // on this request, and clearing the pending entry locally would strand it
    // there forever with the answer undelivered (the "blocked UI" failure).
    // Surface the refusal and leave the modal up so the user can retry.
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot answer: the stream is closing or stalled");
            BumpUiVersion();
        }
        return;
    }

    if (auto state = GetState(session_id)) state->ClearPendingFor(request_id);
}

void AppState::AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                                  const std::string& chosen_option) {
    BumpUiVersion();
    auto stream = GetStream(session_id);
    if (!stream) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot answer: this session is not attached");
        }
        return;
    }
    if (RefuseIfEnded(session_id, "answer")) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* cr = ev.mutable_confirmation_response();
    cr->set_request_id(request_id);
    cr->set_chosen_option(chosen_option);
    // Same refusal rule as AnswerPermission: keep the modal up on a stalled
    // stream rather than pretending the answer went out.
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot answer: the stream is closing or stalled");
            BumpUiVersion();
        }
        return;
    }

    if (auto state = GetState(session_id)) {
        state->ClearPendingFor(request_id);
        // No tool events or message stops follow a command confirmation: the
        // worker was parked on THIS answer, so the tab must re-derive its
        // state here or it stays stranded in Waiting forever.
        state->OnPendingResolved();
    }
}

void AppState::SendCommand(const std::string& session_id, const std::string& line) {
    auto stream = GetStream(session_id);
    if (!stream) {
        // Same rule as SendChatTurn: a `/`-command on a stream-less tab
        // (a refused resume) must say why nothing happened.
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: this session is not attached");
            BumpUiVersion();
        }
        return;
    }
    if (RefuseIfEnded(session_id, "send")) return;

    const auto invocation = SplitCommand(line);
    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* command = ev.mutable_command();
    // Client-generated, echoed on the CommandResult. The counter is per client,
    // not a constant: two commands on two streams must not echo-collide.
    static std::atomic<uint64_t> next_request_id{0};
    command->set_request_id("cmd-" + std::to_string(next_request_id.fetch_add(1)));
    command->set_name(invocation.name);
    for (const auto& value : invocation.positional) {
        command->mutable_args()->add_positional(value);
    }
    command->mutable_args()->set_raw(invocation.raw);
    // A refused send is surfaced, not swallowed -- same rule as SendChatTurn/
    // AnswerPermission/AnswerConfirmation: a `/`-command silently vanishing
    // reads as "the harness ignored me", indistinguishable from it having
    // been accepted and simply producing no visible result.
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: the stream is closing or stalled");
            BumpUiVersion();
        }
    }
}

void AppState::SendCancel(const std::string& session_id) {
    BumpUiVersion();   // the "cancelling…" hint renders this frame, not the next event's
    auto stream = GetStream(session_id);
    if (!stream) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot cancel: this session is not attached");
        }
        return;
    }
    if (RefuseIfEnded(session_id, "cancel")) return;
    // The hint stands until the turn ends (TurnCompleted / Error / session
    // end clear it) -- queuing the send alone would leave the press silent
    // while the server winds down.
    if (auto state = GetState(session_id)) state->RequestCancel();

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    ev.mutable_cancel();
    if (!stream->Send(ev)) {
        // Same ruling as SendCommand/SendChatTurn: an explicit user action
        // must not vanish silently on a stalled stream. (The BumpUiVersion
        // above covered the hint; this surfaces WHY it will never clear.)
        if (auto state = GetState(session_id)) {
            state->OnError("cannot cancel: the stream refused the event");
            BumpUiVersion();
        }
    }
}

void AppState::SendSetRunMode(const std::string& session_id, v1::PermissionRunMode mode) {
    auto stream = GetStream(session_id);
    if (!stream) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot switch permission mode: this session is not attached");
            BumpUiVersion();
        }
        return;
    }
    if (RefuseIfEnded(session_id, "set run mode")) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    ev.mutable_set_run_mode()->set_run_mode(mode);
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot switch permission mode: the stream refused the event");
            BumpUiVersion();
        }
    }
}

void AppState::SendRewind(const std::string& session_id, std::optional<int64_t> target_seq,
                          bool redo) {
    auto stream = GetStream(session_id);
    if (!stream) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: this session is not attached");
            BumpUiVersion();
        }
        return;
    }
    if (RefuseIfEnded(session_id, "rewind")) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* rewind = ev.mutable_rewind();
    if (redo) {
        rewind->set_redo(true);
    } else if (target_seq.has_value()) {
        rewind->set_target_seq(*target_seq);
    } else {
        return;   // neither a target nor redo: nothing to send
    }
    // Same refusal rule as SendCommand/SendChatTurn: a rewind/redo the user
    // explicitly asked for must not vanish silently on a stalled stream.
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: the stream is closing or stalled");
            BumpUiVersion();
        }
    }
}

std::optional<int64_t> AppState::FindRewindTargetSeq(const std::string& session_id, int n) {
    const auto state = GetState(session_id);
    if (!state || n <= 0) return std::nullopt;

    // scrollback_page is DESCENDING by seq (newest at index 0, matching what
    // GetMessages returns -- see ApplyScrollbackPage), so a forward scan
    // already walks from the newest message backward.
    //
    // The predicate is the SERVER's IsRewindTarget (agent/session.cpp),
    // mirrored here field-for-field: role User carrying ANY non-empty
    // TextBlock. Counting bare User rows as well would let /rewind N target
    // an injected subagent completion (User role, tool-result blocks only) --
    // a position the server refuses to rewind to, and one the user could
    // never re-enter as a prompt.
    const auto snapshot = state->TakeSnapshot();
    int remaining = n;
    for (const auto& stored : *snapshot.scrollback_page) {
        if (stored.message().role() != v1::ROLE_USER) continue;
        bool text_bearing = false;
        for (const auto& block : stored.message().content()) {
            if (block.has_text() && !block.text().text().empty()) {
                text_bearing = true;
                break;
            }
        }
        if (!text_bearing) continue;
        if (--remaining == 0) return stored.seq();
    }
    return std::nullopt;
}

void AppState::RequestScrollback(const std::string& session_id, bool newest) {
    // Owning copy up front, captured by value into the job: the fetch now
    // runs on the RPC worker thread, well after this call returns, and can
    // outlive a CloseTab that races it (picker delete / tab close mid-fetch)
    // -- the same reasoning the old synchronous body already relied on, just
    // extended across the thread hop.
    const auto state = GetState(session_id);
    if (!state) return;

    std::optional<int64_t> cursor;
    if (!newest) {
        // In-flight latch FIRST: two "load older" presses inside one RPC round
        // trip fetch the same before_seq page, and the second apply used to
        // replace the window with the older page. While one fetch is
        // outstanding the press no-ops (ApplyScrollbackPage's overlap dedupe
        // is the second layer, for fetches that raced the latch).
        if (!state->TryBeginOlderFetch()) return;
        const auto snapshot = state->TakeSnapshot();
        cursor = NextScrollbackCursor(snapshot.last_loaded_seq, snapshot.has_earlier_messages);
        if (!cursor.has_value()) {
            state->EndOlderFetch();   // history exhausted; nothing to ask
            return;
        }
    }

    // The turn live at submit time (Fix: the persisted page covers THIS turn;
    // a newer one that starts before the apply must keep its live text).
    const uint64_t turn_at_submit = state->CurrentTurnSeq();

    // `this` is captured because BumpUiVersion() is AppState's own private
    // method -- SessionState has no back-reference to bump it, so the
    // original synchronous body called it directly instead. Safe: RunTui
    // joins the worker (StopRpcQueue) before AppState can be destroyed, so
    // no queued job outlives its `this`.
    // must_run: the job resolves SessionState's older_fetch_pending_ latch
    // (opened by TryBeginOlderFetch before this Submit) and the refresh
    // machine's refresh_pending (resolved only by a refresh_seq bump, which
    // only a run produces) -- drop-oldest eviction of this job stranded both
    // forever, so it is protected (rpc_queue.hpp).
    rpc_queue_.Submit([this, state, session_id, newest, cursor, turn_at_submit] {
        v1::GetMessagesResponse resp;
        grpc::ClientContext context;
        // Bounded: the fetch runs off the loop thread now, but a wedged
        // server must not wedge the worker (and therefore every OTHER
        // queued request) indefinitely.
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        v1::GetMessagesRequest request;
        request.set_session_id(session_id);
        if (cursor.has_value()) request.set_before_seq(*cursor);
        request.set_limit(100);
        if (const grpc::Status status = client_->GetMessages(&context, request, &resp);
            !status.ok()) {
            // Resolve the attempt without applying anything: the fetch that
            // opened the mismatch is still open, so the next frame's
            // maintain() sees transcript_version != rendered_version again
            // and retries -- this used to be a dead comment ("the next
            // trigger retries") because the loop-thread caller had already
            // latched rendered_version to the pre-refresh version before
            // this failure was known. The status message rides along: the
            // failure streak's user-visible notice (NoteRefreshFailed) names
            // WHY the transcript stopped moving.
            state->NoteRefreshFailed(status.error_message());
            if (!newest) state->EndOlderFetch();   // the latch resolves on failure too
            BumpUiVersion();
            return;
        }
        state->ApplyScrollbackPage(resp);   // bumps transcript_version + refresh_seq
        if (newest) {
            // Turn-aware: only the turn this fetch was submitted under is
            // provably covered by the persisted page (see
            // ClearStreamingIfCurrentTurn).
            state->ClearStreamingIfCurrentTurn(turn_at_submit);
        } else {
            state->EndOlderFetch();
        }
        BumpUiVersion();
    }, /*must_run=*/true);
}

std::vector<AppState::TabSnapshot> AppState::GetAllTabs(bool include_tool_output) const {
    std::lock_guard lk(mtx_);
    std::vector<TabSnapshot> out;
    out.reserve(tabs_.size());
    for (const auto& tab : tabs_) {
        TabSnapshot snap;
        // LIGHT: no scrollback/streaming copies; tool output only when asked.
        const SessionState::Snapshot ss = tab.info.state->TakeLightSnapshot(include_tool_output);
        snap.tools = ss.tools;
        snap.tab_id = tab.info.tab_id;
        snap.session_id = tab.info.session_id;
        snap.has_session = tab.stream != nullptr;
        // A session with no name yet (before the first turn's default naming
        // event lands) rendered its RAW session id as the tab label -- a
        // 16-char ULID fragment that resize repaints then left behind as
        // junk lines. A stable placeholder reads as "not named yet", which
        // is what it is.
        snap.display_name = ss.display_name.empty() ? std::string("new session") : ss.display_name;
        snap.state = ss.state;
        snap.is_subagent = tab.info.parent_session_id.has_value();
        snap.root_ordinal = tab.info.root_ordinal;
        snap.spawn_seq = tab.info.spawn_seq;
        snap.cancel_requested = ss.cancel_requested;
        // The glyph merge (docs/EXTENSIBILITY.md 4.2, "Attention sink"): core
        // event derivation is the default source; a plugin raise wins until
        // cleared. '!' stays the only color-independent signal whatever raised
        // it, so both sources render the SAME glyph.
        snap.attention_glyph =
            (ss.state == SessionStateKind::Waiting ||
             (plugin_attention_ != nullptr && plugin_attention_->TabRaised(tab.info.session_id)))
                ? '!'
                : '\0';
        snap.is_focused = (focused_id_ == tab.info.tab_id);
        out.push_back(std::move(snap));
    }
    return out;
}

SessionState::Snapshot AppState::GetDetails(const std::string& tab_id) const {
    // The DEEP snapshot for ONE tab (track R's focused-tab pump redesign): the
    // frame renders the focused tab only, so deep-copying every tab per frame
    // -- under each tab's lock -- was pure waste. Keyed by tab_id: the focused
    // handle may be a sessionless tab (whose session_id is still "").
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == tab_id) return tab.info.state->TakeSnapshot();
    }
    return SessionState::Snapshot{};
}

std::string AppState::FocusedSessionId() const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == focused_id_) return tab.info.session_id;
    }
    return std::string{};
}

std::string AppState::FocusedTabId() const {
    std::lock_guard lk(mtx_);
    return focused_id_;
}

void AppState::CloseAllTabs() {
    // Latch BEFORE anything else -- before the tabs_ snapshot and the drain:
    // a subagent spawn landing after the snapshot but before the clear is
    // exactly the attach-we-never-detach race, and the latch is what makes
    // OnSubagentSpawned refuse it (see there).
    shutting_down_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<SessionClient::Stream>> to_close;
    {
        std::lock_guard lk(mtx_);
        // A sessionless tab (no message sent yet) has a null stream -- CloseTab
        // already guards this per-tab; the bulk variants must too, or the first
        // sessionless tab (e.g. the one RunTui opens at startup) crashes here.
        for (auto& tab : tabs_) {
            if (tab.stream) to_close.push_back(tab.stream);
        }
        tabs_.clear();
        focused_id_.clear();
    }
    // Outside the lock: Close() drains and joins the stream's threads.
    for (auto& stream : to_close) stream->Close();
}

void AppState::DetachAllTabs() {
    // S5.4: quit's teardown. Same shape as CloseAllTabs() -- copy the streams
    // out from under the lock, then act on the copies -- but Detach() instead
    // of Close(): every session this client was attached to keeps running
    // server-side (any turn in flight completes unattended), rather than
    // being torn down just because the terminal closed.
    //
    // Latch FIRST (before the tabs_ snapshot): a spawn landing after the
    // snapshot would open a stream no teardown will ever detach -- see
    // OnSubagentSpawned. Spawns racing BEFORE the latch are still drained by
    // the tabs_ copy below (they are already in tabs_).
    shutting_down_.store(true, std::memory_order_release);
    std::vector<std::shared_ptr<SessionClient::Stream>> to_detach;
    {
        std::lock_guard lk(mtx_);
        // Same null guard as CloseAllTabs(): a sessionless tab has no stream.
        for (auto& tab : tabs_) {
            if (tab.stream) to_detach.push_back(tab.stream);
        }
        tabs_.clear();
        focused_id_.clear();
    }
    for (auto& stream : to_detach) stream->Detach();
}

AppState::TabEntry* AppState::FindTab(const std::string& tab_id) {
    for (auto& tab : tabs_) {
        if (tab.info.tab_id == tab_id) return &tab;
    }
    return nullptr;
}

AppState::TabEntry* AppState::FindTabBySession(const std::string& session_id) {
    // A sessionless tab's session_id is "" -- an empty id here must never
    // match it, or every session-keyed call made with an unresolved id (e.g.
    // SendCancel(FocusedSessionId()) when the focused tab is sessionless)
    // resolves to an ARBITRARY sessionless tab and surfaces its error notice
    // on the wrong one.
    if (session_id.empty()) return nullptr;
    for (auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return &tab;
    }
    return nullptr;
}

bool AppState::RefuseIfEnded(const std::string& session_id, const char* verb) {
    const auto state = GetState(session_id);
    if (!state || !state->IsEnded()) return false;
    state->OnError(std::string("cannot ") + verb + ": this session has ended");
    BumpUiVersion();
    return true;
}

std::shared_ptr<SessionClient::Stream> AppState::GetStream(const std::string& session_id) {
    // Same empty-id guard as FindTabBySession: "" must never match a
    // sessionless tab's session_id.
    if (session_id.empty()) return nullptr;
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return tab.stream;   // copy the shared_ptr, hold no state lock
    }
    return nullptr;
}

std::shared_ptr<SessionState> AppState::GetState(const std::string& session_id) const {
    if (session_id.empty()) return nullptr;
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) {
            return tab.info.state;   // owning copy: survives CloseTab erasing the entry
        }
    }
    return nullptr;
}

std::shared_ptr<SessionState> AppState::GetStateByTab(const std::string& tab_id) const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == tab_id) return tab.info.state;
    }
    return nullptr;
}

}  // namespace minimal::client
