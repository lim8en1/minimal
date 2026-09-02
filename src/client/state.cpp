#include "minimal/client/state.hpp"

#include "minimal/client/plugins/registries.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

#include "minimal/client/cli_args.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

// ===========================================================================
// SessionState — one per tab, thread-safe snapshot surface.
// All mutators lock an internal mutex (never held across RPC/IO), so they are
// safe to call from the reader thread while AppState owns its own ordering.
// ===========================================================================

SessionState::SessionState(std::string session_id, std::string name)
    : session_id_(std::move(session_id)), display_name_(std::move(name)) {}

SessionState::Snapshot SessionState::TakeLightSnapshot() {
    std::lock_guard lk(mtx_);
    Snapshot s;
    s.session_id = session_id_;
    s.display_name = display_name_;
    s.state = state_;
    s.tools = tools_;   // bounded (R2); the tools panel renders these
    s.input_tokens = input_tokens_;
    s.output_tokens = output_tokens_;
    s.cancel_requested = cancel_requested_;
    s.model = model_;
    s.agent_preset = agent_preset_;
    s.permission_mode = permission_mode_;
    s.context_window_tokens = context_window_tokens_;
    // Deliberately NOT copied (heavy, and only the focused tab renders them):
    // scrollback_page, streaming_text, pending_permission/confirmation,
    // latest_notice.
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
    s.scrollback_page = scrollback_page_;
    s.has_earlier_messages = has_earlier_;
    s.last_loaded_seq = last_loaded_seq_;
    s.streaming_text = streaming_accumulated_;
    s.is_streaming_finalized = !is_streaming_;   // not streaming => finalized this turn
    if (pending_permission_) {
        s.pending_permission = *pending_permission_;
    }
    if (pending_confirmation_) {
        s.pending_confirmation = *pending_confirmation_;
    }
    s.tools = tools_;
    s.input_tokens = input_tokens_;
    s.output_tokens = output_tokens_;
    s.cancel_requested = cancel_requested_;
    s.transcript_version = transcript_version_;
    s.scrollback_trimmed = scrollback_trimmed_;
    s.turn_seq = turn_seq_;
    s.latest_notice = latest_notice_;
    s.notice_seq = notice_seq_;
    s.model = model_;
    s.agent_preset = agent_preset_;
    s.permission_mode = permission_mode_;
    s.context_window_tokens = context_window_tokens_;
    s.refresh_seq = refresh_seq_;
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

void SessionState::AppendStreamingText(std::string_view text) {
    std::lock_guard lk(mtx_);
    // A NEW TURN begins only after the previous one ENDED (TurnCompleted/Error)
    // -- message stops fire between tool cycles WITHIN one turn, so arming on
    // is_streaming_ alone would re-key the stream mid-turn. (See turn_seq in
    // the Snapshot.)
    if (turn_ended_) {
        ++turn_seq_;
        turn_ended_ = false;
    }
    streaming_accumulated_.append(text);
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

// "Idle | MessageStopEvent, nothing pending" (ARCHITECTURE.md, Tab states and
// attention). A tool-using turn emits several message_stops before the turn is
// actually done -- this fires between tool cycles too, which is correct: the
// tab really is briefly idle there.
void SessionState::MaybeGoIdleLocked() {
    if (state_ == SessionStateKind::Error || state_ == SessionStateKind::Done) return;
    const bool tool_active = !tools_.empty() && tools_.back().started;
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
    // Turn completion is bookkeeping, not a state transition: `Done` means the
    // SESSION ended (see OnSessionEnded), not that a turn finished -- the
    // preceding message_stop already resolved state_ (normally to Idle). An
    // in-flight Error is left alone rather than silently cleared here.
    tools_.clear();   // the panel resets when a new turn begins
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
    is_streaming_ = false;
}

void SessionState::OnError(const std::string&) {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    turn_ended_ = true;   // the errored turn is over; the next stream is a new one
    cancel_requested_ = false;   // a cancel can BE the error path (dead stream)
    pending_permission_.reset();
    pending_confirmation_.reset();
    state_ = SessionStateKind::Error;
}

// The real "subagent session stopped" signal (ARCHITECTURE.md): the session's
// stream ended, not merely a turn. Terminal -- nothing can answer a pending
// modal once the session is gone.
void SessionState::OnSessionEnded() {
    std::lock_guard lk(mtx_);
    is_streaming_ = false;
    cancel_requested_ = false;
    pending_permission_.reset();
    pending_confirmation_.reset();
    state_ = SessionStateKind::Done;
}

void SessionState::OnResyncRequired(const std::string& reason) {
    std::lock_guard lk(mtx_);
    // The streamed material on hand may be missing pieces -- drop the live
    // half (the persisted transcript has the authoritative record) and mark
    // the content changed so the frame pump's newest-slice refresh fires. A
    // pending prompt is NOT dropped: the server replays outstanding prompts
    // on attach (S5.3), and a prompt whose modal is showing is still real.
    streaming_accumulated_.clear();
    is_streaming_ = false;
    ++transcript_version_;
    common::DefaultLogger().info("session {}: resync required ({})", session_id_, reason);
}

// Pending modals -------------------------------------------------------------

void SessionState::SetPermissionRequest(std::unique_ptr<PendingPermission> req) {
    std::lock_guard lk(mtx_);
    pending_permission_ = std::move(*req);
    pending_confirmation_.reset();
    if (state_ == SessionStateKind::Done || state_ == SessionStateKind::Idle) {
        state_ = SessionStateKind::Waiting;
    }
}

void SessionState::SetConfirmationRequest(std::unique_ptr<PendingConfirmation> req) {
    std::lock_guard lk(mtx_);
    pending_confirmation_ = std::move(*req);
    pending_permission_.reset();
    if (state_ == SessionStateKind::Done || state_ == SessionStateKind::Idle) {
        state_ = SessionStateKind::Waiting;
    }
}

void SessionState::ClearPendingFor(std::string_view request_id) {
    std::lock_guard lk(mtx_);
    if (pending_permission_ && pending_permission_->request_id == request_id) {
        pending_permission_.reset();
    }
    if (pending_confirmation_ && pending_confirmation_->request_id == request_id) {
        pending_confirmation_.reset();
    }
}

void SessionState::OnPendingResolved() {
    std::lock_guard lk(mtx_);
    // The worker was parked on the prompt this just answered, so nothing else
    // is in flight and MaybeGoIdleLocked's guards all hold.
    MaybeGoIdleLocked();
}

// Tools ---------------------------------------------------------------------
// v1 runs tools sequentially and streams them start -> output deltas -> finish, keyed by
// the tool_use_id that echoes on every delta. We match on that id rather than a positional.

void SessionState::OnToolStarted(const v1::ToolExecutionEvent& evt) {
    std::lock_guard lk(mtx_);
    ToolExecutionState tool;
    tool.tool_use_id = evt.tool_use_id();
    tool.tool_name = evt.tool_name();
    tool.summary = evt.summary();
    tool.started_at_unix_ms = std::chrono::milliseconds(evt.started_at_unix_ms());
    tools_.push_back(std::move(tool));

    auto& live = tools_.back();   // v1 sequential => the just-started tool is last
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
            if (static_cast<int>(it->buffered_output.size()) > tool_output_buffer_bytes_) {
                static const std::string kMarker =
                    "\n··· live output trimmed -- the full result is in the transcript ···\n";
                const std::size_t head_bytes = static_cast<std::size_t>(tool_output_buffer_bytes_) / 4;
                const std::size_t tail_budget =
                    static_cast<std::size_t>(tool_output_buffer_bytes_) - head_bytes - kMarker.size();
                it->buffered_output =
                    it->buffered_output.substr(0, head_bytes) + kMarker +
                    it->buffered_output.substr(it->buffered_output.size() - tail_budget);
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
            it->started = false;   // no longer current — deltas after this won't match
            return;
        }
    }
}

// Scrollback -----------------------------------------------------------------

void SessionState::ApplyScrollbackPage(const v1::GetMessagesResponse& resp) {
    std::lock_guard lk(mtx_);
    if (resp.messages().empty()) {
        has_earlier_ = resp.has_more();   // no rows, but the server may still have older pages
        ++refresh_seq_;   // a resolved attempt (successful RPC, empty page) -- see NoteRefreshFailed
        return;
    }

    if (scrollback_page_.empty() || resp.messages(0).seq() < last_loaded_seq_) {
        // Older than what we hold: append. The window stays DESCENDING by seq
        // (newest at index 0, matching what GetMessages returns), so an older
        // page's newest slot must land just after the existing back of the
        // window, not at its front.
        scrollback_page_.insert(scrollback_page_.end(), resp.messages().begin(),
                                resp.messages().end());
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
    } else {
        // Newer than what we hold — keep only the contiguous newest slice (bounded memory).
        scrollback_page_.assign(resp.messages().begin(), resp.messages().end());
        scrollback_trimmed_ = false;
    }

    last_loaded_seq_ = scrollback_page_.empty() ? -1 : scrollback_page_.back().seq();
    has_earlier_ = resp.has_more();
    ++transcript_version_;
    ++refresh_seq_;
}

bool SessionState::CanLoadMore() const {
    std::lock_guard lk(mtx_);
    return has_earlier_;
}

void SessionState::NoteRefreshFailed() {
    std::lock_guard lk(mtx_);
    ++refresh_seq_;   // resolved (no content change) -- lets a mismatch retry next frame
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

}  // namespace

AppState::AppState(std::shared_ptr<SessionClient> client) : client_(std::move(client)) {}

void AppState::SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes) {
    if (scrollback_max_messages > 0) scrollback_max_messages_ = scrollback_max_messages;
    if (tool_output_buffer_bytes > 0) tool_output_buffer_bytes_ = tool_output_buffer_bytes;
}

void AppState::DisableChatSurface(const char* reason) {
    chat_surface_broken_ = true;
    if (reason != nullptr) {
        common::DefaultLogger().error(
            "chat surface plugin threw ({}) -- disabled; the core chat view takes over", reason);
    } else {
        common::DefaultLogger().error(
            "chat surface plugin threw a non-standard exception -- disabled; the core chat "
            "view takes over");
    }
}

// Open a brand-new session as a tab. Two-phase: create the stream + session id under no
// app lock (session creation is blocking), register the entry, THEN Resume() the reader so
// the first dispatched event can never race an unregistered tab.
std::string AppState::OpenTab(const SessionClient::CreateOptions& opts) {
    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });

    const auto sid = stream->Open(opts);   // creates session, names bidi (threads off)
    if (!sid) {
        return "";   // server rejected the create (or no server reachable) — drop empty stream
    }

    {   // register under a lock, then drop it before starting the reader thread
        std::lock_guard lk(mtx_);
        TabEntry entry;
        entry.stream = stream;
        entry.info.state = std::make_shared<SessionState>(*sid);
        entry.info.state->SetBounds(
            scrollback_max_messages_ > 0 ? scrollback_max_messages_
                                         : SessionState::kDefaultScrollbackWindowMaxMessages,
            tool_output_buffer_bytes_ > 0 ? tool_output_buffer_bytes_
                                          : SessionState::kDefaultToolOutputBufferBytes);
        entry.info.session_id = *sid;
        entry.info.parent_session_id = std::nullopt;   // root tab (a child gets its parent set)
        entry.info.root_ordinal = ++next_root_ordinal_;   // a fresh group id
        entry.info.spawn_seq = next_spawn_seq_++;

        if (focused_id_.empty()) focused_id_ = *sid;   // focus an empty app on its first tab
        tabs_.push_back(std::move(entry));
    }   // lock released here, before the reader thread can dispatch back into us
    BumpUiVersion();

    stream->SetOnClosed([this, sid = *sid](const grpc::Status& status) {
        if (auto state = GetState(sid)) {
            // The reader's Finish() status distinguishes transport death from a
            // clean server-side close: a crashed server used to render as a
            // happy "Done ✓" tab that silently swallowed typed input. The
            // status RIDES IN on the callback -- reaching back into the stream
            // from here could make this callback the stream's last owner while
            // the reader thread is inside it (see Stream::SetOnClosed).
            if (IsTransportDeath(status)) {
                state->OnError("connection lost: " + status.error_message());
            } else {
                state->OnSessionEnded();
            }
        }
    });
    stream->Resume();   // start threads AFTER unlock — a dispatched event must find this entry
    return *sid;
}

// Attach an existing server-side session as a tab. Same two-phase pattern: name+register,
// then Resume() so its child's events find this entry rather than racing it.
std::string AppState::AttachTab(const std::string& session_id, int64_t parent_ordinal) {
    // Duplicate refusal BEFORE any wire traffic: attaching an already-open
    // session would create a second TabEntry whose events all route to the
    // first (FindTab returns the first match), leaving a permanently dead
    // tab. Picker resumes of an open session are the realistic path.
    {
        std::lock_guard lk(mtx_);
        if (FindTab(session_id) != nullptr) return "";
    }

    auto stream = client_->MakeStream(
        [this](const v1::ServerEvent& e) { DispatchToTab(e.session_id(), e); });

    const auto attached = stream->Attach(session_id);   // names the stream (threads off)
    if (!attached) return "";

    {   // register under a lock, then drop it before starting the reader thread
        std::lock_guard lk(mtx_);
        if (FindTab(session_id) != nullptr) {
            // A duplicate slipped through the race window (two concurrent
            // AttachTab calls -- requires the server to double-emit a
            // SubagentStartedEvent). Cancel the underlying call and drop it:
            // its naming event was never sent, so the server never attached
            // it and cancelling cannot terminate the real session the way
            // Finish()-without-cancel could wedge or a close could destroy.
            stream->Cancel();
            return "";
        }
        TabEntry entry;
        entry.stream = stream;
        entry.info.state = std::make_shared<SessionState>(session_id);
        entry.info.state->SetBounds(
            scrollback_max_messages_ > 0 ? scrollback_max_messages_
                                         : SessionState::kDefaultScrollbackWindowMaxMessages,
            tool_output_buffer_bytes_ > 0 ? tool_output_buffer_bytes_
                                          : SessionState::kDefaultToolOutputBufferBytes);
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
        // resume into a fresh session-less TUI must show the session, not the
        // welcome placeholder.
        if (focused_id_.empty()) focused_id_ = session_id;
        tabs_.push_back(std::move(entry));
    }   // lock released here, before the reader thread can dispatch back into us

    stream->SetOnClosed([this, session_id](const grpc::Status& status) {
        if (auto state = GetState(session_id)) {
            if (IsTransportDeath(status)) {
                state->OnError("connection lost: " + status.error_message());
            } else {
                state->OnSessionEnded();
            }
        }
    });
    stream->Resume();   // start threads AFTER unlock — same guard as OpenTab
    return session_id;
}

bool AppState::HasTab(const std::string& session_id) const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return true;
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
    if (chat_surface_ != nullptr && !chat_surface_broken_) {
        try {
            chat_surface_->OnEvent(event);
        } catch (const std::exception& error) {
            chat_surface_broken_ = true;
            common::DefaultLogger().error(
                "chat surface plugin threw ({}) -- disabled; the core chat view takes over",
                error.what());
        } catch (...) {
            chat_surface_broken_ = true;
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
        target->OnError(event.error().message());
    } else if (event.has_resync_required()) {
        // S5: this stream's delivery had a hole. The durable record is the
        // transcript; bumping the version makes the next maintain() pass
        // refetch the newest scrollback through the async RPC queue (T2).
        target->OnResyncRequired(event.resync_required().reason());
    } else if (event.has_prompt_resolved()) {
        // S5.3: another subscriber answered first. Dismiss this tab's modal
        // for that request_id if it still has one open -- a no-op on the tab
        // that answered, since it already cleared its own on send.
        target->ClearPendingFor(event.prompt_resolved().request_id());
    } else if (event.has_session_metadata()) {
        target->OnSessionMetadata(event.session_metadata());   // refreshes tab label later
    } else if (event.has_stream_event()) {
        const auto& se = event.stream_event();
        if (se.has_text_delta()) {
            target->AppendStreamingText(se.text_delta().text());
        } else if (se.has_content_block_stop() || se.has_message_stop()) {
            target->FinalizeStreamingText();
        }
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
    } else if (event.has_command_result()) {
        target->OnCommandResult(event.command_result());
    } else if (event.has_subagent_started()) {
        std::lock_guard lk(mtx_);   // append only; deferred spawn happens after unlock below
        pending_subagents_.emplace_back(session_id, event.subagent_started());
    }
    // command_result is stored as the tab's transient notice above (routed by
    // its display mode in RunTui); PluginCall data is a plugin's own path.

    // The app lock is released now. Spawn any deferred subagent tabs so child streams attach
    // on their own reader threads without ever holding mtx_ while opening (which could deadlock).
    std::vector<std::pair<std::string, v1::SubagentStartedEvent>> spawned;
    {
        std::lock_guard lk(mtx_);
        std::swap(pending_subagents_, spawned);   // drain under a fresh lock
    }
    for (auto& [parent_id, subevt] : spawned) {
        OnSubagentSpawned(parent_id, subevt);
    }
}

void AppState::OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt) {
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
    AttachTab(evt.child_session_id(), parent_root);   // child inherits the group ordinal

    // A blocking subagent pins its parent's turn, so focus follows it (parent is stuck waiting).
    if (evt.blocking()) SetFocusedTab(evt.child_session_id());
}

void AppState::CloseTab(const std::string& session_id) {
    BumpUiVersion();
    std::shared_ptr<SessionClient::Stream> to_close;
    bool was_focused = false;
    {
        std::lock_guard lk(mtx_);
        for (auto it = tabs_.begin(); it != tabs_.end(); ++it) {
            if (it->info.session_id == session_id) {
                to_close = it->stream;
                was_focused = (focused_id_ == session_id);
                tabs_.erase(it);
                break;
            }
        }
        if (was_focused && !tabs_.empty()) focused_id_ = tabs_.back().info.session_id;
    }
    if (to_close) to_close->Close();   // drains + joins; terminates the session when last open
}

void AppState::SetFocusedTab(const std::string& session_id) {
    BumpUiVersion();
    std::lock_guard lk(mtx_);
    // FindTab, not HasTab: HasTab locks mtx_ itself, and this mutex is not
    // recursive -- calling it here would deadlock every caller, including
    // FocusWithWrap and the blocking-subagent auto-focus path.
    if (FindTab(session_id) != nullptr) focused_id_ = session_id;
}

void AppState::CycleFocusForward() { FocusWithWrap(true); }
void AppState::CycleFocusBackward() { FocusWithWrap(false); }

void AppState::FocusWithWrap(bool forward) {
    std::vector<std::string> order;
    {
        std::lock_guard lk(mtx_);
        for (const auto& tab : tabs_) order.push_back(tab.info.session_id);
    }
    if (order.empty()) return;

    const auto current = std::find(order.begin(), order.end(), focused_id_);
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
    if (!stream) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    ev.mutable_chat_turn()->set_text(text);
    // A refused send is surfaced, not swallowed: the send queue is bounded
    // (the server stopped reading) or the stream is dead -- either way, typed
    // input silently vanishing reads as "the model ignored me".
    if (!stream->Send(ev)) {
        if (auto state = GetState(session_id)) {
            state->OnError("cannot send: the stream is closing or stalled");
        }
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
    if (!stream) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* pr = ev.mutable_permission_response();
    pr->set_request_id(request_id);
    pr->set_decision(decision);
    for (auto& k : granted_keys) pr->add_granted_keys(k);
    stream->Send(ev);

    if (auto state = GetState(session_id)) state->ClearPendingFor(request_id);
}

void AppState::AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                                  const std::string& chosen_option) {
    BumpUiVersion();
    auto stream = GetStream(session_id);
    if (!stream) return;

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    auto* cr = ev.mutable_confirmation_response();
    cr->set_request_id(request_id);
    cr->set_chosen_option(chosen_option);
    stream->Send(ev);

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
    if (!stream) return;

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
    stream->Send(ev);
}

void AppState::SendCancel(const std::string& session_id) {
    BumpUiVersion();   // the "cancelling…" hint renders this frame, not the next event's
    auto stream = GetStream(session_id);
    if (!stream) return;
    // The hint stands until the turn ends (TurnCompleted / Error / session
    // end clear it) -- queuing the send alone would leave the press silent
    // while the server winds down.
    if (auto state = GetState(session_id)) state->RequestCancel();

    v1::ClientEvent ev;
    ev.set_session_id(session_id);
    ev.mutable_cancel();
    stream->Send(ev);
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
        const auto snapshot = state->TakeSnapshot();
        cursor = NextScrollbackCursor(snapshot.last_loaded_seq, snapshot.has_earlier_messages);
        if (!cursor.has_value()) return;   // history exhausted; nothing to ask
    }

    // `this` is captured because BumpUiVersion() is AppState's own private
    // method -- SessionState has no back-reference to bump it, so the
    // original synchronous body called it directly instead. Safe: RunTui
    // joins the worker (StopRpcQueue) before AppState can be destroyed, so
    // no queued job outlives its `this`.
    rpc_queue_.Submit([this, state, session_id, newest, cursor] {
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
            // this failure was known.
            state->NoteRefreshFailed();
            BumpUiVersion();
            return;
        }
        state->ApplyScrollbackPage(resp);   // bumps transcript_version + refresh_seq
        if (newest) state->ClearStreaming();
        BumpUiVersion();
    });
}

std::vector<AppState::TabSnapshot> AppState::GetAllTabs() const {
    std::lock_guard lk(mtx_);
    std::vector<TabSnapshot> out;
    out.reserve(tabs_.size());
    for (const auto& tab : tabs_) {
        TabSnapshot snap;
        const SessionState::Snapshot ss = tab.info.state->TakeLightSnapshot();   // LIGHT: no scrollback/streaming copies
        snap.tools = ss.tools;
        snap.session_id = tab.info.session_id;
        snap.display_name = ss.display_name.empty() ? tab.info.session_id : ss.display_name;
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
        snap.is_focused = (focused_id_ == tab.info.session_id);
        out.push_back(std::move(snap));
    }
    return out;
}

std::map<std::string, SessionState::Snapshot> AppState::GetAllDetails() const {
    std::lock_guard lk(mtx_);
    std::map<std::string, SessionState::Snapshot> out;
    for (const auto& tab : tabs_) {
        out.emplace(tab.info.session_id, tab.info.state->TakeSnapshot());
    }
    return out;
}

SessionState::Snapshot AppState::GetDetails(const std::string& session_id) const {
    // The DEEP snapshot for ONE tab (track R's focused-tab pump redesign): the
    // frame renders the focused tab only, so deep-copying every tab per frame
    // -- under each tab's lock -- was pure waste. GetAllDetails stays for
    // callers that genuinely need every tab.
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return tab.info.state->TakeSnapshot();
    }
    return SessionState::Snapshot{};
}

std::string AppState::FocusedSessionId() const {
    std::lock_guard lk(mtx_);
    return focused_id_;
}

void AppState::CloseAllTabs() {
    std::vector<std::shared_ptr<SessionClient::Stream>> to_close;
    {
        std::lock_guard lk(mtx_);
        for (auto& tab : tabs_) to_close.push_back(tab.stream);
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
    std::vector<std::shared_ptr<SessionClient::Stream>> to_detach;
    {
        std::lock_guard lk(mtx_);
        for (auto& tab : tabs_) to_detach.push_back(tab.stream);
        tabs_.clear();
        focused_id_.clear();
    }
    for (auto& stream : to_detach) stream->Detach();
}

AppState::TabEntry* AppState::FindTab(const std::string& session_id) {
    for (auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return &tab;
    }
    return nullptr;
}

std::shared_ptr<SessionClient::Stream> AppState::GetStream(const std::string& session_id) {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) return tab.stream;   // copy the shared_ptr, hold no state lock
    }
    return nullptr;
}

std::shared_ptr<SessionState> AppState::GetState(const std::string& session_id) const {
    std::lock_guard lk(mtx_);
    for (const auto& tab : tabs_) {
        if (tab.info.session_id == session_id) {
            return tab.info.state;   // owning copy: survives CloseTab erasing the entry
        }
    }
    return nullptr;
}

}  // namespace minimal::client
