#include "minimal/client/state.hpp"

#include "minimal/client/plugins/registries.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <iterator>
#include <utility>

#include "minimal/client/command_line.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/client/policy.hpp"
#include "minimal/common/fnv.hpp"

namespace minimal::client {

namespace {
// The wire spellings of the run mode, as the server renders it into session
// metadata's permission_mode field (server's config/permission_codec.hpp
// ToString -- the client never links that, so the vocabulary is mirrored
// here; a drift would show up as a status bar that never confirms a pending
// switch, not as a wrong mode being enforced).
std::string PermissionModeName(v1::PermissionRunMode mode) {
    switch (mode) {
        case v1::PERMISSION_RUN_MODE_BYPASS: return "bypass";
        case v1::PERMISSION_RUN_MODE_ALLOW_ONLY: return "allow-only";
        default: return "normal";
    }
}
}  // namespace

// ===========================================================================
// SessionState — one per tab, thread-safe snapshot surface.
// All mutators lock an internal mutex (never held across RPC/IO), so they are
// safe to call from the reader thread while AppState owns its own ordering.
// ===========================================================================

SessionState::SessionState(std::string session_id, std::string name)
    : session_id_(std::move(session_id)), display_name_(std::move(name)) {}

// The fields BOTH snapshots carry: identity, derived state, usage, turn
// flags and the status-bar metadata. One list, so a field added for the tab
// bar cannot reach one snapshot and not the other.
void SessionState::FillCommonLocked(Snapshot& s) const {
    s.session_id = session_id_;
    s.display_name = display_name_;
    s.state = state_;
    s.input_tokens = input_tokens_;
    s.output_tokens = output_tokens_;
    s.cache_read_tokens = cache_read_tokens_;
    s.cache_write_tokens = cache_write_tokens_;
    s.cancel_requested = cancel_requested_;
    s.turn_in_flight = turn_in_flight_ || !commands_in_flight_.empty();
    s.turns_started = turns_started_;
    s.turns_ended = turns_ended_;
    s.last_turn_completed = last_turn_completed_;
    s.live_view_truncated = live_.view_truncated();
    s.model = model_;
    s.agent_preset = agent_preset_;
    s.desired_model = desired_model_;
    s.desired_agent_preset = desired_agent_;
    s.desired_permission_mode = desired_permission_mode_;
    s.permission_mode = permission_mode_;
    s.paused = paused_;
    s.context_window_tokens = context_window_tokens_;
}

SessionState::Snapshot SessionState::TakeLightSnapshot(bool include_tool_output) {
    std::lock_guard lk(mtx_);
    Snapshot s;
    FillCommonLocked(s);
    s.tools = tools_.Copy(include_tool_output);
    // Deliberately NOT copied (heavy, and only the focused tab renders them):
    // scrollback_page, the streaming accumulations (has_streaming_text +
    // live_segments' copy-on-publish views instead), pending_permission/
    // confirmation, latest_notice.
    return s;
}

std::string SessionState::FlatStreamingText() const {
    std::lock_guard lk(mtx_);
    return live_.flat_text();
}

std::string SessionState::FlatThinkingText() const {
    std::lock_guard lk(mtx_);
    return live_.flat_thinking();
}

void SessionState::SetBounds(int scrollback_max_messages, int tool_output_buffer_bytes) {
    std::lock_guard lk(mtx_);
    window_.SetMaxMessages(scrollback_max_messages);
    tools_.SetOutputCap(tool_output_buffer_bytes);
}

SessionState::Snapshot SessionState::TakeSnapshot() {
    std::lock_guard lk(mtx_);
    Snapshot s;
    FillCommonLocked(s);
    s.scrollback_page = window_.page_view();   // shared, copy-on-apply
    s.compactions = window_.compactions_view();
    s.pending_user_messages = window_.echoes();
    s.pending_user_text =
        window_.echoes().empty() ? std::string{} : window_.echoes().front().text;
    s.has_earlier_messages = window_.has_earlier();
    s.last_loaded_seq = window_.last_loaded_seq();
    // The flat streaming/thinking accumulations are NOT copied here: they
    // duplicate live_segments' text, they change on every delta, and the
    // per-event deep copy of a growing stream was churn for consumers that
    // only ask "is anything streaming" (has_streaming_text) or render the
    // segments' own copy-on-publish views (live_segments below). See
    // FlatStreamingText/FlatThinkingText for the test seam.
    s.has_streaming_text = live_.has_text();
    s.is_streaming_finalized = !live_.is_streaming();   // not streaming => finalized this turn
    s.live_segments = live_.CopySegments();
    s.pending_permission = prompts_.permission();
    s.pending_confirmation = prompts_.confirmation();
    s.pending_prompt = prompts_.plugin_prompt();
    // LIGHT tool copy: ChatView::RenderLiveToolRow (the only deep-snapshot
    // tool reader) never reads buffered_output -- the running-tools panel is what
    // renders that, off TakeLightSnapshot(true) instead. This used to copy
    // every tool's full (up to 64 KiB) live buffer on every focused-tab deep
    // snapshot for a consumer that ignores it.
    s.tools = tools_.Copy(/*include_output=*/false);
    s.transcript_version = transcript_version_;
    s.fetch_generation = fetch_generation_;
    s.rewind_epoch = window_.rewind_epoch();
    s.turn_ended = live_.turn_ended();
    s.scrollback_trimmed = window_.trimmed();
    s.scrollback_gap = window_.gap();
    s.turn_seq = live_.turn_seq();
    s.latest_notice = latest_notice_;
    s.notice_seq = notice_seq_;
    s.refresh_seq = window_.refresh_seq();
    s.refresh_succeeded = window_.refresh_succeeded();
    return s;
}

void SessionState::OnSessionCreated(std::string name) {
    std::lock_guard lk(mtx_);
    if (!name.empty()) display_name_ = std::move(name);
    state_ = SessionStateKind::Idle;
}

void SessionState::SeedDisplayNameIfUnset(std::string name) {
    std::lock_guard lk(mtx_);
    if (name.empty() || !display_name_.empty()) return;
    display_name_ = std::move(name);
    // The tools panel shows each row's session name (see OnSessionMetadata).
    tools_.BumpVersion();
}

void SessionState::OnSessionMetadata(const v1::SessionMetadataEvent& metadata) {
    std::lock_guard lk(mtx_);
    if (!metadata.name().empty()) {
        if (display_name_ != metadata.name()) {
            // The tools panel shows each row's session name -- a rename must
            // invalidate its cache too (see ToolsVersion).
            tools_.BumpVersion();
        }
        display_name_ = metadata.name();
    }
    // The E5 metadata fields: the status bar's wire source. Empty/0 leaves the
    // previous value alone -- a partial re-emit (a rename event) must not wipe
    // fields it does not carry.
    if (!metadata.model().empty()) model_ = metadata.model();
    if (!metadata.agent_preset().empty()) agent_preset_ = metadata.agent_preset();
    if (!metadata.permission_mode().empty()) {
        permission_mode_ = metadata.permission_mode();
        // The metadata confirmation of a shift+tab: retire the desired
        // state ONLY when the wire mode matches it. Racing presses send two
        // changes; the first confirmation (the intermediate mode) must not
        // clear the pending marker for the second.
        if (!desired_permission_mode_.empty() &&
            desired_permission_mode_ == permission_mode_) {
            desired_permission_mode_.clear();
        }
    }
    if (metadata.context_window_tokens() > 0) {
        context_window_tokens_ = metadata.context_window_tokens();
    }
    // The pause state rides EVERY metadata emission (see the proto comment --
    // partial renames fill it in too), so it applies unconditionally; there
    // is no partial event that leaves it stale.
    paused_ = metadata.paused();
}

void SessionState::SetSessionId(std::string session_id) {
    std::lock_guard lk(mtx_);
    session_id_ = std::move(session_id);
}

void SessionState::SetDesiredModel(std::string model_ref, bool is_default) {
    std::lock_guard lk(mtx_);
    desired_model_ = std::move(model_ref);
    desired_model_default_ = is_default;
}

bool SessionState::DesiredModelIsDefault() const {
    std::lock_guard lk(mtx_);
    return desired_model_default_;
}

void SessionState::SetDesiredAgentPreset(std::string preset_name) {
    std::lock_guard lk(mtx_);
    desired_agent_ = std::move(preset_name);
}

void SessionState::SetDesiredPermissionMode(std::string mode) {
    std::lock_guard lk(mtx_);
    desired_permission_mode_ = std::move(mode);
}

std::string SessionState::DesiredModel() const {
    std::lock_guard lk(mtx_);
    return desired_model_;
}

std::string SessionState::DesiredAgentPreset() const {
    std::lock_guard lk(mtx_);
    return desired_agent_;
}

std::string SessionState::DesiredPermissionMode() const {
    std::lock_guard lk(mtx_);
    return desired_permission_mode_;
}

std::vector<std::string> SessionState::ConvergenceCommands() const {
    std::lock_guard lk(mtx_);
    std::vector<std::string> out;
    // No effective metadata yet: either the session was just created with the
    // desired state riding the request (the metadata event is in flight), or
    // this is a sessionless tab (nothing to converge into). Sending blind
    // switches against unknown effective state would be pure noise.
    if (agent_preset_.empty() && model_.empty()) return out;
    // /agent FIRST: the server's precedence rule has a user-chosen model
    // survive a preset switch, so ordering agent-then-model lands the desired
    // pair exactly. (The reverse order would let /agent re-resolve the
    // preset's own model and drop the /model choice.)
    if (!desired_agent_.empty() && desired_agent_ != agent_preset_) {
        out.push_back("/agent " + desired_agent_);
    }
    if (!desired_model_.empty() && desired_model_ != model_) {
        // "Back to the default" clears the session's choice rather than
        // pinning the default's ref, so a later settings change still applies.
        out.push_back(desired_model_default_ ? std::string("/model default")
                                             : "/model " + desired_model_);
    }
    return out;
}

void SessionState::BeginTurnLocked() {
    if (turn_in_flight_) return;
    turn_in_flight_ = true;
    ++turns_started_;
}

void SessionState::EndTurnLocked(bool completed) {
    if (!turn_in_flight_) return;
    turn_in_flight_ = false;
    ++turns_ended_;
    last_turn_completed_ = completed;
}

bool SessionState::ArmNewTurnLocked() {
    if (!live_.Arm()) return false;
    BeginTurnLocked();
    return true;
}

void SessionState::AppendStreamingText(std::string_view text) {
    std::lock_guard lk(mtx_);
    // The first delta of a NEW turn arms it -- the only arming an INJECTED
    // turn gets (a subagent completion triggers a seedless turn without
    // OnChatSubmitted). "A genuinely new turn just started" (vs. a stray late
    // event for the turn that just errored) is also what authorizes clearing
    // a sticky Error below.
    const bool starting_new_turn = live_.AppendText(text);
    if (starting_new_turn) BeginTurnLocked();
    // A fresh assistant turn supersedes any unanswered prompt (including
    // Waiting -- the prompt is cleared by this same call, so the state must
    // move with it rather than strand in Waiting with nothing pending).
    // Error is normally sticky here too (a stray delta for the turn that just
    // errored must not silently erase the visible error tint) -- EXCEPT when
    // this delta starts a genuinely new turn: the user tried again, and Error
    // must not outlive the turn that caused it.
    if (state_ != SessionStateKind::Error || starting_new_turn) {
        state_ = SessionStateKind::Thinking;
    }
    prompts_.ClearAll();
}

void SessionState::FinalizeStreamingText() {
    std::lock_guard lk(mtx_);
    live_.EndBlock();
    MaybeGoIdleLocked();
}

void SessionState::AppendStreamingThinking(std::string_view text) {
    std::lock_guard lk(mtx_);
    // Same turn arming as AppendStreamingText (thinking typically OPENS the
    // turn, so it is often the first delta to see the last one end). Unlike
    // a text delta, a thinking delta does not clear a pending prompt.
    const bool starting_new_turn = live_.AppendThinking(text);
    if (starting_new_turn) BeginTurnLocked();
    // See AppendStreamingText's identical guard: Error stays sticky within a
    // turn, but must not outlive it.
    if (state_ != SessionStateKind::Error || starting_new_turn) {
        state_ = SessionStateKind::Thinking;
    }
}

// "Idle | MessageStopEvent, nothing pending" (ARCHITECTURE.md, Tab states and
// attention). A tool-using turn emits several message_stops before the turn is
// actually done -- this fires between tool cycles too, which used to resolve
// the tab to Idle there. Now the turn-in-flight signal outranks that: a turn
// whose provider went silent between a message stop and the next event (a
// long prefill, reasoning without deltas) sat Idle for MINUTES while the turn
// was very much alive -- the interrupt probe read it as idle, ESC did
// nothing, and ctrl+c hit the idle-quit path. While turn_in_flight_ stands,
// the tab holds Working (the spinner persists between cycles, which is the
// honest display anyway).
void SessionState::MaybeGoIdleLocked() {
    if (state_ == SessionStateKind::Error || state_ == SessionStateKind::Done) return;
    // ANY tool still running, not just the LAST-started one: ARCHITECTURE.md
    // "Tool dispatch is parallel" makes several tools in flight at once the
    // COMMON case (width 4 by default), and results arrive in COMPLETION
    // order, not gate order -- tools_.back().started alone flipped the tab to
    // Idle (spinner gone, "Working" lost) the moment the last-STARTED tool
    // finished, even with earlier calls of the same round still executing on
    // the pool.
    // Excludes background rows: OnTurnCompleted deliberately KEEPS a live
    // background row across a turn boundary (its launching call already got
    // its own finished ack; the detached process is a separate concern whose
    // terminal BackgroundTaskEvent can arrive many turns later). Counting it
    // here as "still active" made a backgrounded `npm run dev` pin this
    // whole branch off forever -- state_ simply stopped being re-derived by
    // this function for as long as that background task ran.
    const bool tool_active = tools_.AnyForegroundRunning();
    if (!tool_active && !prompts_.Any()) {
        state_ = turn_in_flight_ ? SessionStateKind::Working : SessionStateKind::Idle;
    }
}

void SessionState::OnTurnCompleted(const v1::TurnCompletedEvent& evt) {
    std::lock_guard lk(mtx_);
    live_.EndTurn();
    EndTurnLocked(/*completed=*/true);   // the turn ended, whatever the stop reason
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
    {
        // The turn's live Tool markers outlive this sweep -- they stay until
        // the post-turn refresh retires them against the persisted rows --
        // so each keeps a copy of its row's final state first.
        live_.CaptureFinalTools(tools_.rows());
        tools_.SweepForTurnEnd();   // bumps ToolsVersion when it swept rows
    }
    // A CANCELLED turn (or one that died at a prompt) ends WITHOUT a message
    // stop: the server denied the parked prompt on our behalf
    // (Session::Cancel's pending.CancelAll), so the answer never traveled
    // through AnswerPermission/AnswerConfirmation and the client-side pending
    // request would outlive its turn -- stranding the tab in Waiting with
    // nothing left to answer. A turn cannot complete while its OWN prompt is
    // genuinely outstanding (the worker is blocked on it), so anything pending
    // here is stale. MaybeGoIdleLocked resolves the Waiting the missing
    // message stop would have; it leaves an in-flight Error alone. A plugin prompt is the
    // same shape as permission/confirmation here -- CancelAll covers all
    // three pending kinds identically -- so it resets alongside them; leaving
    // it out stranded the tab Waiting with a dead prompt modal after ESC.
    prompts_.ClearAll();
    MaybeGoIdleLocked();
    // The completed turn's messages are persisted now but NOT yet in the
    // scrollback window; bumping the version is what makes the chat view
    // re-fetch and swap its streaming block for the real rows.
    ++transcript_version_;
    ++fetch_generation_;
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
    commands_in_flight_.erase(evt.request_id());
}

void SessionState::OnCommandSent(const std::string& request_id) {
    std::lock_guard lk(mtx_);
    commands_in_flight_.insert(request_id);
}

bool SessionState::ClearNotice() {
    std::lock_guard lk(mtx_);
    if (!latest_notice_.has_value()) return false;
    latest_notice_.reset();
    ++notice_seq_;
    return true;
}

void SessionState::TickNotice(std::chrono::steady_clock::time_point now) {
    std::lock_guard lk(mtx_);
    if (!latest_notice_.has_value()) return;
    if (latest_notice_->ticks_remaining > 0) --latest_notice_->ticks_remaining;
    if (latest_notice_->ticks_remaining > 0) return;
    if (now - latest_notice_->shown_at < kNoticeMinVisible) return;
    latest_notice_.reset();
    ++notice_seq_;
}

bool SessionState::HasRunningTool() const {
    std::lock_guard lk(mtx_);
    return tools_.AnyRunning();
}

std::optional<std::chrono::milliseconds> SessionState::FastestRunningToolElapsedMs() const {
    std::lock_guard lk(mtx_);
    return tools_.FastestRunningElapsed();
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
    live_.Clear();
}

void SessionState::ClearStreamingIfCurrentTurn(uint64_t turn_seq) {
    std::lock_guard lk(mtx_);
    live_.ClearIfTurn(turn_seq);
}

uint64_t SessionState::CurrentTurnSeq() const {
    std::lock_guard lk(mtx_);
    return live_.turn_seq();
}

bool SessionState::CurrentTurnEnded() const {
    std::lock_guard lk(mtx_);
    return live_.turn_ended();
}

uint64_t SessionState::CurrentRewindEpoch() const {
    std::lock_guard lk(mtx_);
    return window_.rewind_epoch();
}

void SessionState::DiscardStaleScrollbackPage() {
    std::lock_guard lk(mtx_);
    window_.DiscardStale();
}

void SessionState::OnError(const std::string& error_message) {
    std::lock_guard lk(mtx_);
    live_.EndTurn();   // the errored turn is over; the next stream is a new one
    EndTurnLocked(/*completed=*/false);   // dead streams end turns too -- the probe must not latch
    commands_in_flight_.clear();   // nothing will answer them on this stream
    cancel_requested_ = false;   // a cancel can BE the error path (dead stream)
    prompts_.ClearAll();
    // A queued run-mode change whose turn/stream just errored may never reach
    // its apply-time metadata event; the pending marker would outlive the
    // change it advertised. The confirming path (OnSessionMetadata) is the
    // normal retirement; this is the failure path.
    desired_permission_mode_.clear();
    // The turn failed before any page apply could retire the echo; nothing
    // will, so drop it here rather than leaving a ghost of an unacknowledged
    // message above the error.
    window_.ClearEchoes();
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

void SessionState::OnSendFailed(const std::string& error_message) {
    std::lock_guard lk(mtx_);
    // Same transient-notice surface OnError uses, and nothing else: no
    // pending_* reset, no state_ change, no turn bookkeeping. Answer*'s own
    // comment is "leave the modal up so the user can retry" -- OnError's
    // blanket clearing (including the very pending field the modal reads)
    // directly contradicted that, making the prompt the user was trying to
    // answer vanish with nothing left to retry while the server worker
    // stayed parked on it.
    latest_notice_ = CommandNotice{error_message, /*is_error=*/true,
                                   v1::COMMAND_DISPLAY_STATUS, /*view_hint=*/{}};
    ++notice_seq_;
}

// The real "subagent session stopped" signal (ARCHITECTURE.md): the session's
// stream ended, not merely a turn. Terminal -- nothing can answer a pending
// modal once the session is gone.
void SessionState::OnSessionEnded() {
    std::lock_guard lk(mtx_);
    live_.EndTurn();   // terminal: a final newest-fetch may still swap the partial stream
    EndTurnLocked(/*completed=*/false);   // terminal -- the probe must not survive the session
    commands_in_flight_.clear();
    cancel_requested_ = false;
    prompts_.ClearAll();
    window_.ClearEchoes();   // same rule as OnError: nothing will retire it
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
    live_.Clear();
    // A dropped ToolExecutionEvent{finished} is exactly the kind of gap a
    // resync exists to cover: a FOREGROUND row whose finish never arrived
    // stays "running" forever, and (per MaybeGoIdleLocked) blocks the tab
    // from ever resolving off Working. Background rows are left alone, same
    // exception MaybeGoIdleLocked and OnTurnCompleted both already draw:
    // their terminal BackgroundTaskEvent is a real, separate signal this
    // resync does not touch.
    {
        tools_.SweepForResync();   // bumps ToolsVersion when it swept rows
    }
    // Nothing else re-derives state_ after a resync otherwise -- a resync
    // that arrived while the tab looked idle-but-actually-Working (or vice
    // versa) left it stuck reading that way until some unrelated event
    // happened to fire MaybeGoIdleLocked next.
    MaybeGoIdleLocked();
    ++transcript_version_;
    ++fetch_generation_;
    common::DefaultLogger().info("session {}: resync required ({})", session_id_, reason);
}

// SessionRewoundEvent (proto/minimal/v1): the transcript tail was cut (rewind)
// or restored (redo). The durable record moved under the window -- trim it to
// the ACTIVE transcript and drop the live/pending-turn state that belonged to
// the discarded rows. Not a turn terminator, so state_ is left alone.
void SessionState::OnSessionRewound(int64_t truncated_to_seq, int64_t active_last_seq) {
    std::lock_guard lk(mtx_);
    // The window (rows, compactions, echoes, refresh bookkeeping) follows the
    // durable record; the live-state wipe goes with the rows it belonged to,
    // and a pending cancel belongs to the discarded turn.
    window_.Rewind(truncated_to_seq, active_last_seq);
    live_.Clear();
    cancel_requested_ = false;
    // The version bump is the mismatch the refresh state machine refetches.
    ++transcript_version_;
    ++fetch_generation_;
}

// Pending modals -------------------------------------------------------------

void SessionState::SetPermissionRequest(std::unique_ptr<PendingPermission> req) {
    std::lock_guard lk(mtx_);
    prompts_.SetPermission(std::move(*req));
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
    prompts_.SetConfirmation(std::move(*req));
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Waiting;
    }
}

void SessionState::SetPluginPrompt(std::unique_ptr<PendingPluginPrompt> req) {
    std::lock_guard lk(mtx_);
    prompts_.SetPluginPrompt(std::move(*req));
    if (state_ != SessionStateKind::Error) {
        state_ = SessionStateKind::Waiting;
    }
}

bool SessionState::ClearPendingFor(std::string_view request_id) {
    std::lock_guard lk(mtx_);
    return prompts_.ClearFor(request_id);
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
    // and MaybeGoIdleLocked would wait on it. Ignore the redelivery whether
    // the existing row is still running OR already finished; this handler
    // never legitimately reopens one (background rows reopen through
    // OnBackgroundTask, not here).
    if (tools_.Has(evt.tool_use_id())) return;
    // A NEW TURN can open on a tool call with no preceding text or thinking
    // delta -- a tool-first turn, or an injected turn (a subagent completion
    // triggers one with no OnChatSubmitted at all, same as the delta paths'
    // "seedless turn" case). Without this arming, the tool's live segment
    // kept the PREVIOUS turn's turn_seq_, so that turn's later
    // ClearStreamingIfCurrentTurn retired this turn's own tool marker along
    // with it, and turn_in_flight_ was never armed -- the tab dropped to
    // Idle between tool cycles. Mirrors the delta paths' identical arming,
    // including clearing BOTH flat buffers: consuming turn_ended_ here means
    // whichever delta path sees the next event for this turn no longer finds
    // turn_ended_ set, and never gets to clear its own buffer's leftover.
    const bool starting_new_turn = ArmNewTurnLocked();
    tools_.Start(evt);
    // Track C4: a marker in the SAME arrival-order timeline text segments
    // use, so the live view can render this tool call between the text
    // that preceded and followed it instead of after everything.
    live_.PushToolMarker(evt.tool_use_id());
    // Same Error-stickiness guard as the delta paths: Error stays sticky
    // within a turn, but must not outlive it -- including when a tool call,
    // not a delta, is what opens the new turn.
    if (state_ == SessionStateKind::Idle || state_ == SessionStateKind::Done ||
        (state_ == SessionStateKind::Error && starting_new_turn)) {
        state_ = SessionStateKind::Working;
    }
}

void SessionState::OnToolOutputDelta(const std::string& tool_use_id, const std::string& chunk) {
    if (chunk.empty()) return;   // ignore empty writes to avoid a spurious space
    std::lock_guard lk(mtx_);
    tools_.OutputDelta(tool_use_id, chunk);
}

void SessionState::OnToolFinished(const std::string& tool_use_id, const v1::ToolExecutionEvent& final_evt) {
    std::lock_guard lk(mtx_);
    if (tools_.Finish(tool_use_id, final_evt) == ToolRunTracker::Finished::Foreground) {
        // A finished foreground tool round is exactly when the server has the
        // round's rows persisted (the assistant message at response
        // completion, the results after the pool drains) -- bump the version
        // so the frame pump fetches and the content-matched retirement in
        // ApplyScrollbackPage swaps the finished live rows for the real
        // persisted ones MID-TURN, instead of holding everything live until
        // the turn ends. Background rows stay out of it: their result row
        // retires nothing (the live row outlives the ack).
        ++transcript_version_;
        ++fetch_generation_;
    }
}

void SessionState::OnBackgroundTask(const v1::BackgroundTaskEvent& evt) {
    std::lock_guard lk(mtx_);
    if (!tools_.BackgroundTask(evt)) return;
    // Reopened: the same C4 marker OnToolStarted pushes -- a reopened
    // background row (its own STARTED never reached this client, or the row
    // already finished and was swept) is still live content for whatever
    // turn is current now.
    live_.PushToolMarker(evt.tool_use_id());
}

// Scrollback -----------------------------------------------------------------

void SessionState::OnCompaction(const v1::CompactionEvent& evt) {
    std::lock_guard lk(mtx_);
    window_.MergeCompaction(evt);
    // The status bar reads last-turn occupancy; the compaction just replaced
    // it. The server's after-figure is an estimate, but so is any number the
    // bar could show before the next response reports real usage.
    input_tokens_ = evt.tokens_after();
    output_tokens_ = 0;
    cache_read_tokens_ = 0;
    cache_write_tokens_ = 0;
    ++transcript_version_;
    ++fetch_generation_;
}

void SessionState::ApplyScrollbackPage(const v1::GetMessagesResponse& resp, bool delta) {
    std::lock_guard lk(mtx_);
    const ScrollbackWindow::Coverage coverage = window_.ApplyPage(resp, delta);
    if (!coverage.applied) return;   // an empty page: resolved, nothing changed
    // Content-matched retirement: the rows just applied prove what is now
    // persisted, so live segments whose content they carry retire here rather
    // than waiting for the turn-end refresh. This is what makes a MID-TURN
    // apply safe (a tool round's finished rows swap in while the turn keeps
    // streaming): the page covers exactly the finished rounds' segments, and
    // anything still open or running stays live below them. The turn-end
    // refresh's coarse clear (ClearStreamingIfCurrentTurn, called by the
    // fetcher after this) remains the belt-and-suspenders for anything the
    // exact match cannot name.
    if (!coverage.answered_tool_ids.empty() || !coverage.assistant_text.empty() ||
        !coverage.assistant_thinking.empty()) {
        live_.Retire(coverage.answered_tool_ids, coverage.assistant_text,
                     coverage.assistant_thinking,
                     [this](const std::string& tool_use_id) { return tools_.Done(tool_use_id); });
    }
    ++transcript_version_;
}

void SessionState::OnChatSubmitted(const std::string& text) {
    std::lock_guard lk(mtx_);
    // FIFO of echoes: a second submit while the agent is still working is
    // QUEUED server-side (runs after the current turn), and only the oldest
    // echo surviving meant every later queued message was invisible -- and
    // unaddressable by a per-message cancel -- for the whole wait.
    window_.SubmitEcho(text);
    // The turn is in flight from this instant, whatever the display state
    // does -- UNCONDITIONALLY, not only from Idle: a submit right after a
    // Done/Error session must still be interruptible before the first wire
    // event arrives. (The state transition below stays conditional; it is a
    // display choice, and Done/Error mean something to the renderer.)
    BeginTurnLocked();
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

void SessionState::OnTurnQueued(std::uint64_t turn_id, const std::string& text) {
    std::lock_guard lk(mtx_);
    window_.EchoQueued(turn_id, text);
}

void SessionState::OnTurnCanceled(std::uint64_t turn_id, bool removed) {
    std::lock_guard lk(mtx_);
    if (window_.EchoCanceled(turn_id, removed)) {
        // The click lost the race: the turn had already been popped and must
        // run. Say so -- a silent press reads as "the message was cancelled"
        // when it was not.
        latest_notice_ = CommandNotice{"queued message already started; it was not cancelled",
                                       /*is_error=*/true, v1::COMMAND_DISPLAY_STATUS,
                                       /*view_hint=*/{}};
        ++notice_seq_;
    }
}

bool SessionState::CanLoadMore() const {
    std::lock_guard lk(mtx_);
    return window_.has_earlier();
}

int64_t SessionState::NewestScrollbackSeq() const {
    std::lock_guard lk(mtx_);
    return window_.NewestSeq();
}

std::string SessionState::SessionId() const {
    std::lock_guard lk(mtx_);
    return session_id_;
}

void SessionState::NoteRefreshFailed(const std::string& reason) {
    std::lock_guard lk(mtx_);
    // Retry PACING is scrollback_refresh.hpp's per-tab backoff; the streak
    // only decides when the silence ends. A server that refuses every page
    // (an undecodable transcript row answers INTERNAL) used to retry forever
    // with no user-visible symptom -- a transcript frozen mid-conversation.
    // Fire ONCE per streak (==, not >=): further failures extend the backoff
    // window but must not spam the notice line every retry.
    const int failures = window_.NoteFailure();
    if (failures == kScrollbackFailureNoticeAfter) {
        latest_notice_ = CommandNotice{
            "transcript refresh failed " + std::to_string(failures) + " times in a row" +
                (reason.empty() ? "" : ": " + reason) +
                "; still retrying -- the transcript shown may be stale",
            /*is_error=*/true, v1::COMMAND_DISPLAY_STATUS, /*view_hint=*/{}};
        ++notice_seq_;
    }
}

bool SessionState::TryBeginOlderFetch() {
    std::lock_guard lk(mtx_);
    return window_.TryBeginOlderFetch();
}

void SessionState::EndOlderFetch() {
    std::lock_guard lk(mtx_);
    window_.EndOlderFetch();
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
AppState::~AppState() {
    // Per StopRpcQueue()'s own documented ordering contract (state.hpp): a
    // queued RPC job's completion callback touches SessionState/AppState the
    // same way a reader-thread dispatch does, so the worker must stop before
    // CloseAllTabs() tears down the tabs a straggling job would publish into.
    StopRpcQueue();
    CloseAllTabs();
}

void AppState::SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes) {
    if (scrollback_max_messages > 0) scrollback_max_messages_ = scrollback_max_messages;
    if (tool_output_buffer_bytes > 0) tool_output_buffer_bytes_ = tool_output_buffer_bytes;
}

void AppState::ApplyUiBounds(SessionState& state) const {
    state.SetBounds(
        scrollback_max_messages_ > 0 ? scrollback_max_messages_
                                     : minimal::client::kDefaultScrollbackWindowMaxMessages,
        tool_output_buffer_bytes_ > 0 ? tool_output_buffer_bytes_
                                      : minimal::client::kDefaultToolOutputBufferBytes);
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

// The startup --resume path for a LIVE session (see the header): root tab
// first, then its live subagent children as members of the same group. The
// child scan runs AFTER the root attaches, both because the children need the
// root's ordinal to group under and because a probe-then-attach caller can
// only get fresher is_live data this way.
std::string AppState::AttachSessionGroup(const std::string& session_id) {
    const std::string root_tab = AttachTab(session_id);
    if (root_tab.empty()) return "";

    // The group's own project_root governs where its children live -- not
    // this client's cwd -- so the scan lists all dirs and filters by parent
    // here. Paged with the same (updated_at, id) cursor rule ListSessions
    // documents, until a short page says there is no more.
    std::vector<std::string> live_children;
    SessionClient::ListSessionsOptions opts;
    opts.include_all_dirs = true;
    opts.include_subagents = true;
    const auto listed = client_->ListAllSessions(opts, [&](const v1::SessionSummary& row) {
        return row.parent_session_id() == session_id && row.is_live();
    });
    if (!listed.status.ok()) {
        // The root tab is already open and usable; a failed listing must not
        // undo it. The group opens with whatever children were listed.
        common::DefaultLogger().warn("AttachSessionGroup({}): child listing failed: {}",
                                     session_id, listed.status.error_message());
    }
    for (const auto& row : listed.sessions) live_children.push_back(row.session_id());

    // Children inherit the ROOT's ordinal, and only the root's: AttachTab's
    // grouping matches the group leader by "this ordinal, no parent" -- the
    // same lookup OnSubagentSpawned does for the parent before attaching.
    int64_t root_ordinal = 0;
    {
        std::lock_guard lk(mtx_);
        for (const auto& tab : tabs_) {
            if (tab.info.tab_id == root_tab) {
                root_ordinal = tab.info.root_ordinal;
                break;
            }
        }
    }
    for (const auto& child : live_children) {
        AttachTab(child, root_ordinal);
    }
    return root_tab;
}

// A READ-ONLY transcript tab for a stored session (see the header): the
// refused-resume configuration -- TabInfo + SessionState with a REAL session
// id and a null stream -- which the scrollback machine already renders
// (AdvanceScrollbackRefresh fetches a fresh tab's newest page on its first
// frame; GetMessages is a unary RPC keyed by session id with a database-only
// existence check, so a finished subagent's transcript pages exactly like a
// live one's). What this method adds is the guard rail: `view_only` keeps
// every submission path from treating the tab as sessionless and minting a
// new session over the transcript being viewed.
std::string AppState::OpenTranscriptTab(const std::string& session_id,
                                        const std::string& parent_session_id) {
    {
        std::lock_guard lk(mtx_);
        // Duplicate rule (AttachTab/ResumeTab): one tab per session -- two
        // tabs would both page the same transcript, and DispatchToTab routes
        // events to the first match anyway.
        if (FindTabBySession(session_id) != nullptr) return "";
    }

    std::string registered;
    {
        std::lock_guard lk(mtx_);
        if (FindTabBySession(session_id) != nullptr) return "";   // race window re-check
        TabEntry entry;
        entry.stream = nullptr;   // the whole point: no stream, no CreateSession
        entry.info.state = std::make_shared<SessionState>(session_id);
        ApplyUiBounds(*entry.info.state);
        entry.info.tab_id = "t" + std::to_string(++next_tab_seq_);
        entry.info.session_id = session_id;
        entry.info.view_only = true;
        // Group with the parent when it is open here (the subagent view
        // shows as a "↳" child and closes with the group, matching every
        // other child tab); a closed parent must not fail the open, so the
        // transcript then stands as its own root.
        bool grouped = false;
        if (!parent_session_id.empty()) {
            for (const auto& tab : tabs_) {
                if (tab.info.session_id == parent_session_id) {
                    entry.info.parent_session_id = parent_session_id;
                    entry.info.root_ordinal = tab.info.root_ordinal;
                    grouped = true;
                    break;
                }
            }
        }
        if (!grouped) {
            entry.info.parent_session_id = std::nullopt;
            entry.info.root_ordinal = ++next_root_ordinal_;
        }
        entry.info.spawn_seq = next_spawn_seq_++;
        if (focused_id_.empty()) focused_id_ = entry.info.tab_id;   // OpenTab's rule
        registered = entry.info.tab_id;
        tabs_.push_back(std::move(entry));
    }
    BumpUiVersion();

    // Seed the label from the stored row, off the loop thread: a subagent
    // row is created nameless server-side, but a renamed one (or one a
    // titler named) should show its stored name, and GetSession also
    // validates the row exists. Before it lands the tab reads "new session",
    // the same placeholder every unnamed tab shows. This tab receives no
    // metadata events (no stream), so this fetch is the only name source.
    RunOnRpcQueue([this, session_id] {
        const auto found = client_->GetSession(session_id);
        if (!found.status.ok()) {
            common::DefaultLogger().warn("transcript tab {}: row lookup failed: {}", session_id,
                                         found.status.error_message());
            return;
        }
        if (!found.summary.name().empty()) {
            if (auto state = GetState(session_id)) {
                state->OnSessionCreated(found.summary.name());
                BumpUiVersion();
            }
        }
    });
    return registered;
}

bool AppState::HasTab(const std::string& tab_id) const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == tab_id) return true;
    }
    return false;
}

bool AppState::HasQueuedWork() const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.stream == nullptr && !tab.pending.empty()) return true;
    }
    return false;
}

// Reader-thread callback: route a ServerEvent to its owned tab. Defers subagent creation
// until the app lock is released, so opening a child stream never happens while held.
void AppState::DispatchToTab(const std::string& session_id, const v1::ServerEvent& event) {
    // Every wire event can change what the frame renders; bumped on the way
    // out, once the event has been applied.
    const UiVersionBumpOnExit bump{*this};
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
    } else if (event.has_turn_queued()) {
        // A submitted turn was accepted onto the server's work queue: assign
        // its id to the matching optimistic echo, making that message
        // addressable by a per-message cancel while it waits.
        target->OnTurnQueued(event.turn_queued().turn_id(), event.turn_queued().text());
    } else if (event.has_turn_canceled()) {
        target->OnTurnCanceled(event.turn_canceled().turn_id(), event.turn_canceled().removed());
    } else if (event.has_compaction()) {
        target->OnCompaction(event.compaction());
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
        pp->reason = pr.reason();
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
    } else if (event.has_plugin_prompt()) {
        const auto& wire = event.plugin_prompt();
        auto pp = std::make_unique<PendingPluginPrompt>();
        pp->request_id = wire.request_id();
        pp->kind = wire.kind();
        pp->payload = nlohmann::json::parse(wire.payload_json(), nullptr,
                                            /*allow_exceptions=*/false);
        if (pp->payload.is_discarded()) pp->payload = nullptr;
        target->SetPluginPrompt(std::move(pp));
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
    // natural title; seed it ONLY while the tab is still unnamed, so a real
    // SessionMetadataEvent name always wins. The seed never touches the
    // child's derived state, so a racing live turn is not disturbed.
    if (!evt.task().empty()) {
        if (auto child_state = GetState(evt.child_session_id())) {
            child_state->SeedDisplayNameIfUnset(evt.task());
        }
    }

    // A blocking subagent pins its parent's turn, so focus follows it (parent is stuck waiting).
    if (evt.blocking() && !child_tab.empty()) SetFocusedTab(child_tab);
}

void AppState::CloseTab(const std::string& tab_id) {
    const UiVersionBumpOnExit bump{*this};
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
    // Bare drop (the server reads it as a DETACH), off the loop thread --
    // see closer_.
    // Inline once the closer has stopped (shutdown): nothing may leak.
    if (to_close && !closer_.Submit([to_close] { to_close->Close(); }, /*must_run=*/true)) {
        to_close->Close();
    }
}

void AppState::EndTabSessions(const std::vector<std::string>& tab_ids, EndSessionMode mode) {
    // Same shape as CloseTab/DetachAllTabs: copy the streams out from under
    // the lock -- Detach() drains and joins the writer (bounded), which must
    // never happen under mtx_ (nor, now, on the loop thread).
    std::vector<std::shared_ptr<SessionClient::Stream>> to_end;
    {
        std::lock_guard lk(mtx_);
        for (const auto& tab_id : tab_ids) {
            for (auto& tab : tabs_) {
                if (tab.info.tab_id == tab_id && tab.stream) {
                    to_end.push_back(tab.stream);
                    break;
                }
            }
        }
    }
    // Off the loop thread (see closer_), ahead of the Close a following
    // CloseTab queues for the same stream.
    for (auto& stream : to_end) {
        if (!closer_.Submit([stream, mode] { stream->Detach(mode); }, /*must_run=*/true)) {
            stream->Detach(mode);   // shutdown: the closer has stopped
        }
    }
}

void AppState::SetFocusedTab(const std::string& tab_id) {
    const UiVersionBumpOnExit bump{*this};
    std::lock_guard lk(mtx_);
    // FindTab, not HasTab: HasTab locks mtx_ itself, and this mutex is not
    // recursive -- calling it here would deadlock every caller, including
    // the blocking-subagent auto-focus path.
    if (FindTab(tab_id) != nullptr) focused_id_ = tab_id;
}


void AppState::SetTabFilter(TabFilter filter) {
    std::lock_guard lk(mtx_);
    if (tab_filter_ == filter) return;   // same value: no repaint churn
    tab_filter_ = filter;
    BumpUiVersion();
}

TabFilter AppState::ActiveTabFilter() const {
    std::lock_guard lk(mtx_);
    return tab_filter_;
}

void AppState::SendChatTurn(const std::string& session_id, const std::string& text) {
    // A refused or stream-less send is surfaced (SendOnStream): typed input
    // silently vanishing reads as "the model ignored me".
    if (!SendOnStream(session_id, "send", [&](v1::ClientEvent& ev) {
            ev.mutable_chat_turn()->set_text(text);
            return true;
        })) {
        return;
    }

    // OPTIMISTIC ECHO. The persisted user row only reaches a page through a
    // fetch, and mid-turn it is not even persisted yet (the server appends
    // the seed message when the queued turn RUNS, not at submit) -- so a
    // submitted message did not render until the turn ENDED, after the
    // assistant's streaming reply had been visible for the whole turn (the
    // reported "assistant renders before user hi"). Echo the text now; the
    // server-side @-mention expansion (attachments render differently from
    // raw text) arrives with the persisted row and retires the echo.
    if (auto state = GetState(session_id)) {
        state->OnChatSubmitted(text);
        BumpUiVersion();
    }
    // And fetch eagerly: with the echo covering the latency, this is what
    // swaps in the authoritative expanded row within one round trip instead
    // of at turn end. Safe MID-TURN too (a submit while the agent is still
    // working): the apply's content-matched retirement only ever retires
    // live segments the page proves persisted.
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
    bool view_only = false;
    {
        std::lock_guard lk(mtx_);
        if (TabEntry* tab = FindTab(tab_id); tab != nullptr) {
            if (tab->info.view_only) {
                // A read-only transcript tab (OpenTranscriptTab). Falling
                // through here would enqueue the text on tab->pending and
                // EnsureSession would MINT A NEW SESSION, repointing this
                // tab's session_id over the transcript being viewed -- the
                // one hole a null-stream tab had. Refuse visibly instead.
                view_only = true;
            } else if (tab->stream != nullptr && !tab->draining) {
                // `draining` keeps the fast path closed until the create-time
                // backlog is on the wire -- a send racing the drain would
                // reorder FIFO submission order (see TabEntry::draining).
                sessioned = tab->info.session_id;
            } else {
                tab->pending.push_back([this, text](const std::string& sid) {
                    SendChatTurn(sid, text);
                });
            }
        }
    }
    if (view_only) {
        TabNotice(tab_id, "read-only transcript: nothing can be sent to a finished subagent");
        return;
    }
    if (!sessioned.empty()) {
        // DESIRED-STATE CONVERGENCE. The user's last /agent //model choice
        // (or picker selection) is applied HERE, at the turn boundary, by
        // submitting at most two commands ahead of the turn on the same
        // stream -- FIFO ordering lands them at the server's turn boundary
        // before the turn runs, never between a tool_use and its
        // tool_result. Selection itself never touches a live session, so
        // swapping models fifteen times between turns costs zero server
        // traffic; only the final value is ever sent. A failed switch (an
        // unknown ref) surfaces as its command-error notice while the turn
        // proceeds on the model already in force.
        if (const auto state = GetState(sessioned)) {
            for (const auto& line : state->ConvergenceCommands()) {
                SendCommand(sessioned, line);
            }
        }
        SendChatTurn(sessioned, text);
        return;
    }
    RunEnsure(tab_id);
}

void AppState::SubmitCommand(const std::string& tab_id, const std::string& line) {
    // Symmetric with OnChatSubmitted's own clear: a stale notice from a
    // PREVIOUS command (an old /help listing, a stale /cost table) has
    // nothing to say about the one the user is submitting now. Before the
    // locked block below, not after: GetStateByTab takes mtx_ itself.
    if (auto state = GetStateByTab(tab_id)) state->ClearNotice();
    std::string sessioned;
    bool view_only = false;
    {
        std::lock_guard lk(mtx_);
        if (TabEntry* tab = FindTab(tab_id); tab != nullptr) {
            // Same refusal as SubmitChatTurn: a view-only tab must never
            // reach RunEnsure (the session-minting path).
            if (tab->info.view_only) {
                view_only = true;
            } else if (tab->stream != nullptr && !tab->draining) {
                sessioned = tab->info.session_id;
            } else {
                tab->pending.push_back([this, line](const std::string& sid) {
                    SendCommand(sid, line);
                });
            }
        }
    }
    if (view_only) {
        TabNotice(tab_id, "read-only transcript: nothing can be sent to a finished subagent");
        return;
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
    bool view_only = false;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) return;
        if (tab->info.view_only) {
            view_only = true;   // same refusal as SubmitChatTurn/SubmitCommand
        } else if (tab->stream != nullptr && !tab->draining) {
            // The `draining` gate, same as SubmitChatTurn/SubmitCommand: an
            // action must not overtake the create-time backlog still being
            // drained onto the wire.
            sessioned = tab->info.session_id;
        } else {
            tab->pending.push_back(std::move(action));
        }
    }
    if (view_only) {
        TabNotice(tab_id, "read-only transcript: nothing can be sent to a finished subagent");
        return;
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
        // DESIRED-STATE OVERLAY. The user's /agent //model choices (typed or
        // picker-selected on this tab, whenever made before the create
        // resolved) ride the CreateSessionRequest: the session is BORN on
        // them instead of being created on defaults and switched afterwards.
        // A model named on a RESUME is legal server-side too (an override is
        // a property of this invocation, not of the stored row); a preset is
        // not -- the stored row governs it, so a desired preset on a resumed
        // session converges at the first turn instead (SubmitChatTurn).
        const auto desired_model = tab->info.state->DesiredModel();
        const auto desired_agent = tab->info.state->DesiredAgentPreset();
        if (tab->info.state->DesiredModelIsDefault()) {
            opts.model.clear();  // no override: the session runs default_model
            opts.remembered_selection = false;
        } else if (!desired_model.empty()) {
            opts.model = desired_model;
            // A choice made now is a decision, not a preference: refused
            // when it does not resolve, never silently dropped.
            opts.remembered_selection = false;
        }
        if (!desired_agent.empty() && opts.resume_session_id.empty()) {
            opts.agent_preset = desired_agent;
            opts.remembered_selection = false;
        }
    }

    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });
    const auto sid = stream->Open(opts);   // creates session, names bidi (threads off)

    bool tab_gone = false;      // closed while the create was in flight
    bool create_failed = false; // the create was refused (or no server reachable)
    bool dropped_agent = false; // a remembered pick the server did not honor
    bool dropped_model = false;
    SelectionHooks selection_hooks;
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
                if (opts.remembered_selection) {
                    // The server started without whatever part of the
                    // remembered pick no longer resolves: say so once, and
                    // forget it (below, outside the lock).
                    const auto& created = stream->created();
                    dropped_agent = !opts.agent_preset.empty() &&
                                    created.agent_preset() != opts.agent_preset;
                    dropped_model = !opts.model.empty() && created.model() != opts.model;
                    if (dropped_agent || dropped_model) {
                        std::string what;
                        if (dropped_agent) what = "agent '" + opts.agent_preset + "'";
                        if (dropped_model) {
                            what += (what.empty() ? "" : " and ") + std::string("model '") +
                                    opts.model + "'";
                        }
                        v1::CommandResult notice;
                        notice.set_status(v1::COMMAND_STATUS_OK);
                        notice.set_message("your last pick (" + what +
                                           ") is no longer available here; started with the "
                                           "defaults");
                        tab->info.state->OnCommandResult(notice);
                    }
                    selection_hooks = selection_hooks_;
                }
                // Close the fast path BEFORE the backlog drains: a submit
                // landing between this unlock and the drained sends would
                // otherwise overtake them (reordered FIFO submissions -- the
                // server would start the LATER message's turn first and steer
                // the backlog into it). Reopened when pending runs dry, below.
                tab->draining = true;
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
    if ((dropped_agent || dropped_model) && selection_hooks.dropped) {
        selection_hooks.dropped(dropped_agent, dropped_model);
    }

    WireOnClosed(*stream, *sid);
    stream->Resume();   // start threads AFTER unlock — a dispatched event must find this entry
    // The drain LOOPS: anything submitted while `draining` is set lands on
    // tab->pending instead of overtaking the backlog, and each pass sweeps it
    // in arrival order. The flag only clears under mtx_ once a pass found
    // pending empty -- from that instant, direct sends cannot reorder against
    // anything (the queue is gone).
    for (bool more = true; more;) {
        for (auto& action : drained) action(*sid);
        drained.clear();
        {
            std::lock_guard lk(mtx_);
            TabEntry* tab = FindTab(tab_id);
            if (tab == nullptr) break;   // closed mid-drain: pending dies with the tab
            more = !tab->pending.empty();
            if (more) {
                drained.swap(tab->pending);
            } else {
                tab->draining = false;
            }
        }
    }
    return *sid;
}

bool AppState::StageModel(const std::string& tab_id, const std::string& model_ref,
                          bool is_default) {
    SelectionHooks hooks;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) return false;
        // The same desired state on every tab (see the header): EnsureSession
        // overlays it onto the create request, and SubmitChatTurn converges a
        // live session to it at the next turn boundary.
        tab->info.state->SetDesiredModel(model_ref, is_default);
        hooks = selection_hooks_;
    }
    BumpUiVersion();
    if (hooks.picked) hooks.picked(std::nullopt, is_default ? std::string() : model_ref);
    return true;
}

bool AppState::StageAgentPreset(const std::string& tab_id, const std::string& preset_name) {
    SelectionHooks hooks;
    {
        std::lock_guard lk(mtx_);
        TabEntry* tab = FindTab(tab_id);
        if (tab == nullptr) return false;
        tab->info.state->SetDesiredAgentPreset(preset_name);
        hooks = selection_hooks_;
    }
    BumpUiVersion();
    if (hooks.picked) hooks.picked(preset_name, std::nullopt);
    return true;
}

void AppState::SetSelectionHooks(SelectionHooks hooks) {
    std::lock_guard lk(mtx_);
    selection_hooks_ = std::move(hooks);
}

void AppState::RunSessionlessCommand(const std::string& tab_id, const std::string& line) {
    const auto state = GetStateByTab(tab_id);
    if (!state) return;
    // Same "the user has moved on" clear SubmitCommand does for the sessioned
    // path -- this is the sessionless one (bare /sessions, /model, /agent
    // before a session exists).
    state->ClearNotice();
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

bool AppState::ClearTabNotice(const std::string& tab_id) {
    if (auto state = GetStateByTab(tab_id)) {
        if (!state->ClearNotice()) return false;
        BumpUiVersion();
        return true;
    }
    return false;
}

void AppState::TickTabNotice(const std::string& tab_id) {
    if (auto state = GetStateByTab(tab_id)) state->TickNotice();
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

void AppState::RequestSuggestPathsForRoot(const std::string& project_root,
                                          const std::string& prefix, int32_t limit,
                                          std::function<void(std::vector<std::string>)> on_result) {
    rpc_queue_.Submit([this, project_root, prefix, limit, on_result = std::move(on_result)] {
        const auto result = client_->SuggestPathsForRoot(project_root, prefix, limit);
        std::vector<std::string> paths;
        if (result.status.ok()) {
            paths.assign(result.response.paths().begin(), result.response.paths().end());
        }
        if (on_result) on_result(std::move(paths));
        BumpUiVersion();   // wake the pump so the editor's next poll sees it
    });
}

void AppState::RequestListCommands(
    const std::string& session_id, const std::string& project_root,
    std::function<void(std::optional<v1::ListCommandsResponse>)> on_result) {
    rpc_queue_.Submit([this, session_id, project_root, on_result = std::move(on_result)] {
        auto result = client_->ListCommands(project_root, session_id);
        if (on_result) {
            on_result(result.status.ok()
                          ? std::optional<v1::ListCommandsResponse>(std::move(result.response))
                          : std::nullopt);
        }
        BumpUiVersion();   // wake the pump so the next frame reads the new catalog
    });
}

void AppState::AnswerPermission(const std::string& session_id, const std::string& request_id,
                                v1::PermissionDecision decision,
                                std::vector<std::string> granted_keys, std::string deny_message,
                                bool stop_turn) {
    // A refused send must NOT close the prompt: the server worker is parked
    // on this request, and clearing the pending entry locally would strand it
    // there forever with the answer undelivered (the "blocked UI" failure) --
    // hence kKeepPending (OnSendFailed: a notice, the modal stays up for a
    // retry). Only keys the request offered are answered; the server filters
    // extras too.
    if (!SendOnStream(session_id, "answer", [&](v1::ClientEvent& ev) {
            auto* pr = ev.mutable_permission_response();
            pr->set_request_id(request_id);
            pr->set_decision(decision);
            for (auto& k : granted_keys) pr->add_granted_keys(k);
            pr->set_deny_message(std::move(deny_message));
            pr->set_stop_turn(stop_turn);
            return true;
        }, SendRefusal::kKeepPending)) {
        return;
    }
    // The server collects every permission for a round before starting any
    // tool, so no tool event or message stop necessarily follows THIS answer:
    // without the re-derive the tab stayed Waiting for the whole tool run.
    ResolvePending(session_id, request_id);
}

void AppState::AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                                  const std::string& chosen_option) {
    // Same refusal rule as AnswerPermission.
    if (!SendOnStream(session_id, "answer", [&](v1::ClientEvent& ev) {
            auto* cr = ev.mutable_confirmation_response();
            cr->set_request_id(request_id);
            cr->set_chosen_option(chosen_option);
            return true;
        }, SendRefusal::kKeepPending)) {
        return;
    }
    // The worker was parked on THIS answer: nothing else re-derives the tab.
    ResolvePending(session_id, request_id);
}

void AppState::AnswerPluginPrompt(const std::string& session_id, const std::string& request_id,
                                  v1::PromptOutcome outcome, const std::string& answer_json) {
    // Same refusal rule as AnswerPermission.
    if (!SendOnStream(session_id, "answer", [&](v1::ClientEvent& ev) {
            auto* response = ev.mutable_plugin_prompt_response();
            response->set_request_id(request_id);
            response->set_outcome(outcome);
            if (outcome == v1::PROMPT_OUTCOME_ANSWERED) response->set_answer_json(answer_json);
            return true;
        }, SendRefusal::kKeepPending)) {
        return;
    }
    ResolvePending(session_id, request_id);
}

void AppState::SendCommand(const std::string& session_id, const std::string& line) {
    // Refusals are surfaced: a `/`-command silently vanishing reads as
    // "the harness ignored me", indistinguishable from no visible result.
    SendOnStream(session_id, "send", [&](v1::ClientEvent& ev) {
        const auto invocation = SplitCommand(line);
        auto* command = ev.mutable_command();
        // Client-generated, echoed on the CommandResult (the client's one
        // counter, shared with the headless path).
        command->set_request_id(client_->NextCommandRequestId());
        command->set_name(invocation.name);
        for (const auto& value : invocation.positional) {
            command->mutable_args()->add_positional(value);
        }
        command->mutable_args()->set_raw(invocation.raw);
        if (auto state = GetState(session_id)) state->OnCommandSent(command->request_id());
        return true;
    });
}

void AppState::SendCancel(const std::string& session_id) {
    SendOnStream(session_id, "cancel", [&](v1::ClientEvent& ev) {
        // The "cancelling…" hint stands until the turn ends (TurnCompleted /
        // Error / session end clear it) -- the send alone would leave the
        // press silent while the server winds down.
        if (auto state = GetState(session_id)) state->RequestCancel();
        // include_queued: the interrupt takes the queued (not yet started)
        // messages with it -- ESC means "stop everything you were going to
        // do", and each dropped turn announces itself (TurnCanceledEvent)
        // so the echoes come off the transcript.
        ev.mutable_cancel()->set_include_queued(true);
        return true;
    });
}

void AppState::SendCancelQueuedTurn(const std::string& session_id, std::uint64_t turn_id) {
    SendOnStream(session_id, "cancel the queued message", [&](v1::ClientEvent& ev) {
        ev.mutable_cancel_queued_turn()->set_turn_id(turn_id);
        return true;
    });
}

void AppState::SendCancelQueuedTurnByTab(const std::string& tab_id, std::uint64_t turn_id) {
    if (const auto state = GetStateByTab(tab_id)) {
        SendCancelQueuedTurn(state->SessionId(), turn_id);
    }
}

void AppState::SendSetRunMode(const std::string& session_id, v1::PermissionRunMode mode) {
    if (!SendOnStream(session_id, "switch permission mode", [&](v1::ClientEvent& ev) {
            ev.mutable_set_run_mode()->set_run_mode(mode);
            return true;
        })) {
        return;
    }
    // The server applies the change immediately (an atomic flip, like
    // SetPaused) and confirms with a metadata event; until that lands, the
    // requested mode is the tab's desired state -- what the status bar
    // renders with a pending marker and what the next shift+tab advances
    // from.
    if (auto state = GetState(session_id)) {
        state->SetDesiredPermissionMode(PermissionModeName(mode));
    }
}

void AppState::SendSetPaused(const std::string& session_id, bool paused) {
    // No desired-state marker: the server applies this on its reader thread
    // (an atomic flip, no turn-boundary wait), and the confirming metadata
    // event is already in flight.
    SendOnStream(session_id, paused ? "pause" : "resume", [&](v1::ClientEvent& ev) {
        ev.mutable_set_paused()->set_paused(paused);
        return true;
    });
}

void AppState::SendRewind(const std::string& session_id, std::optional<int64_t> target_seq,
                          bool redo) {
    SendOnStream(session_id, redo ? "redo" : "rewind", [&](v1::ClientEvent& ev) {
        auto* rewind = ev.mutable_rewind();
        if (redo) {
            rewind->set_redo(true);
        } else if (target_seq.has_value()) {
            rewind->set_target_seq(*target_seq);
        } else {
            return false;   // neither a target nor redo: nothing to send
        }
        return true;
    });
}

// The /pause // /resume guard. The lookup is TAB-keyed (GetDetails matches
// tab.info.tab_id): the state of the session the user is LOOKING at, which is
// what "already paused" means. It used to be fed FocusedSessionId() -- a ULID
// can never equal a "t<n>" tab id, so the lookup always returned a default
// snapshot reading paused=false, making typed /resume on a paused session
// answer "not paused" and never send the unpause (alt+p was unaffected: its
// dispatcher passes the focused TAB id). The wire event itself is SESSION-
// keyed, like every other send.
AppState::PauseToggle AppState::RequestPaused(bool want_paused) {
    const std::string tab_id = FocusedTabId();
    if (tab_id.empty()) return PauseToggle::NoSession;
    const std::string session = FocusedSessionId();
    if (session.empty()) return PauseToggle::NoSession;
    if (GetDetails(tab_id).paused == want_paused) {
        return want_paused ? PauseToggle::AlreadyPaused : PauseToggle::AlreadyRunning;
    }
    SendSetPaused(session, want_paused);
    return want_paused ? PauseToggle::SentPause : PauseToggle::SentResume;
}

std::optional<int64_t> AppState::FindRewindTargetSeq(const std::string& session_id, int n) {
    const auto state = GetState(session_id);
    if (!state || n <= 0) return std::nullopt;

    // scrollback_page is DESCENDING by seq (newest at index 0, matching what
    // GetMessages returns -- see ApplyScrollbackPage), so a forward scan
    // already walks from the newest message backward.
    //
    // The predicate is the SERVER's IsRewindTarget (agent/transcript_rewinder.cpp),
    // mirrored here field-for-field: role User carrying ANY non-empty
    // TextBlock. Counting bare User rows as well would let /rewind N target
    // an injected subagent completion (User role, tool-result blocks only) --
    // a position the server refuses to rewind to, and one the user could
    // never re-enter as a prompt.
    const auto snapshot = state->TakeSnapshot();
    // A trimmed window's front is NOT the true newest message (the sliding
    // window evicts from the newest end once scrollback_max_messages_ is
    // exceeded by older-page loads) -- counting "the last n user turns" off
    // it can name a turn far older than the user asked for, and /rewind
    // enacts whatever target this returns. Refuse rather than guess wrong.
    if (snapshot.scrollback_trimmed) return std::nullopt;
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
    // A turn-end refresh coarsely clears the covered turns' live material
    // after the apply (ClearStreamingIfCurrentTurn); a MID-TURN fetch -- a
    // tool round finishing while the turn continues -- must not: its apply's
    // content-matched retirement (ApplyScrollbackPage) is the only safe
    // mechanism there, and the coarse clear of the OLD code was exactly what
    // made a mid-turn submit finalize every live row into its collapsed
    // persisted form while the agent was still working. The turn-end bump's
    // own refresh (OnTurnCompleted) always follows with the coarse clear.
    const bool turn_ended_at_submit = state->CurrentTurnEnded();
    // Delta-fetch cursor: the newest seq the window holds. A newest refresh
    // with a non-empty window asks only for rows past this (an append-only
    // transcript makes seq an exact content address -- no window re-send);
    // -1 (nothing held) falls back to the plain newest page. Captured once;
    // a racing apply only makes some returned rows duplicates, which the
    // apply's merge skips.
    const int64_t delta_cursor = newest ? state->NewestScrollbackSeq() : -1;
    // A rewind/redo between submit and apply means this response describes a
    // transcript OnSessionRewound has already trimmed away -- applying it
    // would re-insert the discarded rows. See CurrentRewindEpoch's header
    // comment.
    const uint64_t rewind_epoch_at_submit = state->CurrentRewindEpoch();

    // `this` is captured because BumpUiVersion() is AppState's own private
    // method -- SessionState has no back-reference to bump it, so the
    // original synchronous body called it directly instead. Safe: RunTui
    // joins the worker (StopRpcQueue) before AppState can be destroyed, so
    // no queued job outlives its `this`.
    // must_run: the job resolves the window's load-older latch
    // (opened by TryBeginOlderFetch before this Submit) and the refresh
    // machine's refresh_pending (resolved only by a refresh_seq bump, which
    // only a run produces) -- drop-oldest eviction of this job stranded both
    // forever, so it is protected (rpc_queue.hpp).
    rpc_queue_.Submit([this, state, session_id, newest, cursor, turn_at_submit,
                       turn_ended_at_submit, delta_cursor, rewind_epoch_at_submit] {
        // Delta mode: page forward (after_seq) while the server says more
        // newer rows remain -- a turn-end catch-up after a long absence can
        // exceed one page. Bounded: 50 pages = 5000 rows per job; a longer
        // backlog finishes over subsequent refreshes instead of one
        // unbounded worker occupation.
        const bool delta = newest && delta_cursor >= 0;
        int64_t after = delta_cursor;
        // Whether every page this job asked for actually applied. The turn-end
        // coarse clear below is only sound when the persisted rows replacing
        // the live ones are in the window -- clearing after a failed fetch
        // blanked the turn's live rows until a retry happened to succeed.
        bool applied_all = true;
        for (int page = 0; page < kMaxScrollbackPagesPerRefresh; ++page) {
            // Bounded (GetMessagesPage's deadline): a wedged server must not
            // wedge the worker, and every OTHER queued request with it.
            v1::GetMessagesRequest request;
            request.set_session_id(session_id);
            if (cursor.has_value()) {
                request.set_before_seq(*cursor);
            } else if (delta) {
                request.set_after_seq(after);
            }
            request.set_limit(kScrollbackPageSize);
            auto fetched = client_->GetMessagesPage(request);
            const v1::GetMessagesResponse& resp = fetched.response;
            if (const grpc::Status& status = fetched.status; !status.ok()) {
                // Resolve the attempt without applying anything: the fetch that
                // opened the mismatch is still open, so the next frame's
                // maintain() sees fetch_generation != fetched_generation again
                // and retries -- this used to be a dead comment ("the next
                // trigger retries") because the loop-thread caller had already
                // latched the rendered state to the pre-refresh version before
                // this failure was known. The status message rides along: the
                // failure streak's user-visible notice (NoteRefreshFailed) names
                // WHY the transcript stopped moving.
                state->NoteRefreshFailed(status.error_message());
                applied_all = false;
                break;
            }
            if (state->CurrentRewindEpoch() != rewind_epoch_at_submit) {
                // Superseded by a rewind/redo since this fetch was submitted:
                // OnSessionRewound already produced the authoritative window --
                // applying this stale response would silently undo it (re-insert
                // rows the rewind just cut, or hide the redo's restored ones
                // behind a page keyed to seqs that no longer mean what they did).
                state->DiscardStaleScrollbackPage();
                applied_all = false;
                break;
            }
            state->ApplyScrollbackPage(resp, /*delta=*/delta);   // bumps transcript_version + refresh_seq
            if (!delta) break;
            // Continue only while this page came back full (has_more on an
            // after_seq response = more NEWER rows) and non-empty.
            if (resp.messages().empty() || !resp.has_more()) break;
            after = resp.messages().rbegin()->seq();   // ascending: last row is the newest
        }
        if (newest && turn_ended_at_submit && applied_all) {
            // Turn-end refresh only: coarsely retire the covered turns' live
            // material. A mid-turn fetch (turn_ended_at_submit == false)
            // relies on the apply's content-matched retirement alone -- see
            // the capture site above.
            state->ClearStreamingIfCurrentTurn(turn_at_submit);
        }
        if (!newest) state->EndOlderFetch();
        BumpUiVersion();
    }, /*must_run=*/true);
}

uint64_t AppState::ToolsVersion() const {
    // A hash over (tab, version) pairs, not a plain sum: after a tab closes,
    // the survivors' bumps could walk a sum back to a value the panel cache
    // already holds (same tab count too), and the cache served a stale panel.
    std::lock_guard lk(mtx_);
    uint64_t h = common::kFnv1a64OffsetBasis;
    const auto mix = [&h](uint64_t v) {
        h = common::Fnv1a64(std::string_view(reinterpret_cast<const char*>(&v), sizeof(v)), h);
    };
    for (const auto& tab : tabs_) {
        mix(std::hash<std::string>{}(tab.info.tab_id));
        mix(tab.info.state->ToolsVersion());
    }
    return h;
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
        snap.view_only = tab.info.view_only;
        snap.root_ordinal = tab.info.root_ordinal;
        snap.spawn_seq = tab.info.spawn_seq;
        snap.cancel_requested = ss.cancel_requested;
        snap.paused = ss.paused;
        snap.turn_in_flight = ss.turn_in_flight;
        snap.turns_started = ss.turns_started;
        snap.turns_ended = ss.turns_ended;
        snap.last_turn_completed = ss.last_turn_completed;
        snap.model = ss.model;
        snap.agent_preset = ss.agent_preset;
        snap.permission_mode = ss.permission_mode;
        snap.live_view_truncated = ss.live_view_truncated;
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

AppState::TabSelection AppState::SelectionFor(const std::string& tab_id) const {
    std::lock_guard lk(mtx_);
    TabSelection out;
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id != tab_id) continue;
        out.exists = true;
        out.sessioned = !tab.info.session_id.empty();
        out.project_root = tab.pending_create.project_root;
        out.agent = tab.info.state->DesiredAgentPreset();
        if (out.agent.empty()) out.agent = tab.pending_create.agent_preset;
        out.model_default = tab.info.state->DesiredModelIsDefault();
        out.model = tab.info.state->DesiredModel();
        if (out.model.empty() && !out.model_default) out.model = tab.pending_create.model;
        break;
    }
    return out;
}

AppState::CommandScope AppState::FocusedCommandScope() const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id != focused_id_) continue;
        if (!tab.info.session_id.empty()) return {tab.info.session_id, {}};
        return {{}, tab.pending_create.project_root};
    }
    return {};
}

std::string AppState::FocusedTabId() const {
    std::lock_guard lk(mtx_);
    return focused_id_;
}

bool AppState::FocusedTabIsSubagent() const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.tab_id == focused_id_) return tab.info.parent_session_id.has_value();
    }
    return false;
}

bool AppState::FocusedTabHasRunningTool() const {
    // GetStateByTab takes mtx_ itself: resolve the tab id first, then release
    // before that lookup, same ordering FocusedTabId's callers already use.
    const std::string tab_id = FocusedTabId();
    if (tab_id.empty()) return false;
    const auto state = GetStateByTab(tab_id);
    return state && state->HasRunningTool();
}

std::optional<std::chrono::milliseconds> AppState::FocusedTabFastestRunningToolElapsedMs() const {
    const std::string tab_id = FocusedTabId();
    if (tab_id.empty()) return std::nullopt;
    const auto state = GetStateByTab(tab_id);
    if (!state) return std::nullopt;
    return state->FastestRunningToolElapsedMs();
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

bool AppState::SendOnStream(const std::string& session_id, const std::string& what,
                            const std::function<bool(v1::ClientEvent&)>& fill,
                            SendRefusal refusal) {
    // Every outcome can change what the frame shows (a notice, a hint, a
    // closed modal): one bump, after the change.
    const UiVersionBumpOnExit bump{*this};
    auto stream = GetStream(session_id);
    if (!stream) {
        // A tab with no stream (a refused resume) must say why nothing
        // happened -- a silent return reads as "ignored".
        if (auto state = GetState(session_id)) {
            state->OnError("cannot " + what + ": this session is not attached");
        }
        return false;
    }
    if (RefuseIfEnded(session_id, what.c_str())) return false;
    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    if (!fill(ev)) return false;
    // A refused send: the bounded send queue is full (the server stopped
    // reading) or the stream is dead. Surfaced, never swallowed.
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            const std::string message = "cannot " + what + ": the stream is closing or stalled";
            if (refusal == SendRefusal::kKeepPending) {
                state->OnSendFailed(message);
            } else {
                state->OnError(message);
            }
        }
        return false;
    }
    return true;
}

void AppState::ResolvePending(const std::string& session_id, const std::string& request_id) {
    if (auto state = GetState(session_id)) {
        state->ClearPendingFor(request_id);
        state->OnPendingResolved();
    }
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
