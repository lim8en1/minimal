#include "minimal/client/session_client.hpp"

#include <chrono>
#include <utility>

#include <grpcpp/create_channel.h>

namespace minimal::client {

namespace {
// How long the writer waits for a new event before re-checking closing_. Bounds how
// long Close() takes once triggered.
inline constexpr std::chrono::milliseconds kWriterWakeup = std::chrono::milliseconds(50);
// @-mention suggestions run on the frame loop while the user types -- a wedged
// server must cost the editor a bounded pause, not the UI (this was the one
// unary call with no deadline; every other caller bounds its fetches).
inline constexpr std::chrono::milliseconds kSuggestPathsDeadline = std::chrono::milliseconds(250);
}  // namespace

// ---------------------------------------------------------------------------
// SessionClient (shared channel + stub for headless single-turn plumbing)
// ---------------------------------------------------------------------------

SessionClient::SessionClient(std::string server_address)
    : server_address_(std::move(server_address)),
      channel_(grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials())),
      stub_(v1::SessionService::NewStub(channel_)) {}

SessionClient::~SessionClient() {
    // Cancel any live PluginSubscribe readers so their Read()s return instead
    // of racing this destruction.
    std::lock_guard lk(subscribe_mtx_);
    for (auto* context : active_subscribes_) context->TryCancel();
}

bool SessionClient::WaitForConnected(std::chrono::milliseconds timeout) {
    return channel_->WaitForConnected(std::chrono::system_clock::now() + timeout);
}

std::shared_ptr<SessionClient::Stream> SessionClient::MakeStream(EventCallback cb) {
    // Not make_shared: Stream's constructor is private, and friendship does not
    // extend through std::allocate_shared's internals -- only a `new` expression
    // written in SessionClient's own code (this function) is covered by the
    // `friend class SessionClient;` declaration.
    return std::shared_ptr<Stream>(new Stream(channel_, std::move(cb)));
}

SessionClient::CreateResult SessionClient::CreateSession(const CreateOptions& options) {
    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    if (!options.resume_session_id.empty()) {
        // Resume: the stored session keeps its name, preset, and project_root.
        // Sending them would be refused server-side, so they are not sent.
        request.set_session_id(options.resume_session_id);
    } else {
        request.set_project_root(options.project_root);
        request.set_agent_preset(options.agent_preset);
        request.set_name(options.name);
    }
    // Sent either way -- see CreateOptions::model.
    request.set_model(options.model);
    request.set_run_mode(options.run_mode);

    v1::CreateSessionResponse response;
    const grpc::Status status = stub_->CreateSession(&context, request, &response);
    return CreateResult{response.session().session_id(), status};
}

SessionClient::GetMessagesResult SessionClient::GetMessages(const std::string& session_id,
                                                             std::optional<int64_t> before_seq,
                                                             int32_t limit) {
    grpc::ClientContext context;
    v1::GetMessagesRequest request;
    request.set_session_id(session_id);
    if (before_seq) request.set_before_seq(*before_seq);
    request.set_limit(limit);

    GetMessagesResult result;
    result.status = stub_->GetMessages(&context, request, &result.response);
    return result;
}

grpc::Status SessionClient::GetMessages(grpc::ClientContext* context,
                                        const v1::GetMessagesRequest& request,
                                        v1::GetMessagesResponse* response) {
    return stub_->GetMessages(context, request, response);
}

SessionClient::ListSessionsResult SessionClient::ListSessions(const std::string& project_root,
                                                              bool include_all_dirs,
                                                              int32_t limit) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::ListSessionsRequest request;
    request.set_project_root(project_root);
    request.set_include_all_dirs(include_all_dirs);
    request.set_limit(limit);

    ListSessionsResult result;
    result.status = stub_->ListSessions(&context, request, &result.response);
    return result;
}

SessionClient::ListCommandsResult SessionClient::ListCommands() {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    ListCommandsResult result;
    result.status = stub_->ListCommands(&context, v1::ListCommandsRequest{}, &result.response);
    return result;
}

grpc::Status SessionClient::DeleteSession(const std::string& session_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::DeleteSessionRequest request;
    request.set_session_id(session_id);
    v1::DeleteSessionResponse response;
    return stub_->DeleteSession(&context, request, &response);
}

SessionClient::PluginCallResult SessionClient::PluginCall(const std::string& plugin_name,
                                                          const std::string& method,
                                                          const std::string& json_payload) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    v1::PluginCallRequest request;
    request.set_plugin_name(plugin_name);
    request.set_method(method);
    request.set_json_payload(json_payload);
    PluginCallResult result;
    v1::PluginCallResponse response;
    result.status = stub_->PluginCall(&context, request, &response);
    if (result.status.ok()) result.json_payload = response.json_payload();
    return result;
}

SubscribeOutcome SessionClient::PluginSubscribe(
    const std::string& plugin_name, const std::string& method, const std::string& json_payload,
    const std::function<bool(const std::string& json_payload)>& on_payload,
    grpc::Status* status) {
    grpc::ClientContext context;
    // No deadline: a subscription lives as long as the subscriber does. The
    // escape hatch is on_payload returning false, which cancels the call.
    // Registered for teardown: a subscriber still blocked in Read() at client
    // destruction is unblocked by ~SessionClient's cancel sweep instead of
    // racing destruction.
    {
        std::lock_guard lk(subscribe_mtx_);
        active_subscribes_.insert(&context);
    }
    v1::PluginCallRequest request;
    request.set_plugin_name(plugin_name);
    request.set_method(method);
    request.set_json_payload(json_payload);
    auto reader = stub_->PluginSubscribe(&context, request);
    v1::PluginCallResponse response;
    while (reader->Read(&response)) {
        if (on_payload && !on_payload(response.json_payload())) {
            context.TryCancel();
            {
                std::lock_guard lk(subscribe_mtx_);
                active_subscribes_.erase(&context);
            }
            return SubscribeOutcome::kCancelled;
        }
    }
    const grpc::Status final_status = reader->Finish();
    {
        std::lock_guard lk(subscribe_mtx_);
        active_subscribes_.erase(&context);
    }
    if (status != nullptr) *status = final_status;
    return final_status.ok() ? SubscribeOutcome::kEnded : SubscribeOutcome::kError;
}

SessionClient::SuggestPathsResult SessionClient::SuggestPaths(const std::string& session_id,
                                                              const std::string& prefix,
                                                              int32_t limit) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kSuggestPathsDeadline);
    v1::SuggestPathsRequest request;
    request.set_session_id(session_id);
    request.set_prefix(prefix);
    request.set_limit(limit);

    SuggestPathsResult result;
    result.status = stub_->SuggestPaths(&context, request, &result.response);
    return result;
}

grpc::Status SessionClient::RunTurn(
    const std::string& session_id, const std::string& text,
    const std::function<bool(const ServerEvent&)>& on_event,
    const std::function<void(const std::shared_ptr<TurnHandle>&)>& on_handle) {
    grpc::ClientContext context;
    auto stream = stub_->OpenSession(&context);

    // The first ClientEvent is what names the session server-side and attaches this
    // stream to it -- OpenSession carries no request message of its own.
    v1::ClientEvent turn;
    turn.set_session_id(session_id);
    turn.mutable_chat_turn()->set_text(text);
    if (!stream->Write(turn)) {
        stream->WritesDone();
        return stream->Finish();
    }

    // Published (on THIS thread) before the first read so a cancel can land at
    // any point of the turn. The handle is cleared before RunTurn returns,
    // which turns every later Cancel into a no-op rather than a write on a
    // finished stream.
    auto control = std::make_shared<TurnControl>();
    control->cancel_event.set_session_id(session_id);
    control->cancel_event.mutable_cancel();
    if (on_handle) {
        on_handle(std::make_shared<TurnHandle>(control, TurnHandle::Token{}));
    }
    {
        std::lock_guard<std::mutex> lock(control->mu);
        control->stream = stream.get();
    }

    v1::ServerEvent event;
    bool half_closed = false;
    while (stream->Read(&event)) {
        if (!half_closed && event.has_permission_request()) {
            // RunTurn write-closes nothing but sends no answers either -- this
            // client cannot respond to a mid-turn permission prompt, and the
            // server worker would block on one forever. Auto-deny instead of
            // wedging: the headless path forces AllowOnly at the session level
            // (app.cpp), so reaching here means a non-conforming run-mode
            // arrived somehow -- degrade loudly, never hang.
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_DENY);
            std::lock_guard<std::mutex> lock(control->mu);
            stream->Write(answer);
        }
        bool done = false;
        if (!half_closed) {
            // TurnCompletedEvent is the ONLY reliable end-of-turn signal. A
            // tool-using turn emits several message_deltas before it is actually
            // finished, so stop_reason alone would half-close mid-turn.
            done = !on_event(event) || event.has_turn_completed();
        }
        if (done && !half_closed) {
            {
                // Same mutex as TurnHandle::Cancel's Write: gRPC's sync stream
                // allows one write-side operation at a time, and a Cancel that
                // lands exactly here raced the bare WritesDone.
                std::lock_guard<std::mutex> lock(control->mu);
                stream->WritesDone();
            }
            half_closed = true;
            // Keep reading. Half-closing says we are done SENDING; it does not
            // entitle us to stop receiving, and abandoning the read side would
            // wedge the server's writer mid-Write().
        }
    }
    {
        // stream is nulled under the same lock, so a late Cancel no-ops and
        // this final WritesDone cannot overlap one.
        std::lock_guard<std::mutex> lock(control->mu);
        if (!half_closed) stream->WritesDone();
        control->stream = nullptr;
    }
    return stream->Finish();
}

grpc::Status SessionClient::RunCommand(
    const std::string& session_id, const CommandOptions& options,
    const std::function<bool(const v1::ServerEvent&)>& on_event) {
    grpc::ClientContext context;
    auto stream = stub_->OpenSession(&context);

    // Named client-side. The wire carries structured args precisely so the
    // server never re-splits a command line: this side is the one holding the
    // text a human typed.
    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    auto* command = invocation.mutable_command();
    // A per-client counter, not a constant: two commands on different streams
    // of the same client would echo-collide on a shared "cmd-1" if a future
    // change ever multiplexed them.
    command->set_request_id(
        "cmd-" + std::to_string(command_counter_.fetch_add(1, std::memory_order_relaxed) + 1));
    command->set_name(options.name);
    for (const auto& value : options.positional) command->mutable_args()->add_positional(value);
    command->mutable_args()->set_raw(options.raw);
    if (!stream->Write(invocation)) {
        stream->WritesDone();
        return stream->Finish();
    }

    v1::ServerEvent event;
    bool half_closed = false;
    while (stream->Read(&event)) {
        if (!half_closed && event.has_confirmation_request()) {
            // Answered on the stream, mid-flight. The server's worker is blocked
            // in RequestConfirmation until this lands.
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_confirmation_response();
            response->set_request_id(event.confirmation_request().request_id());
            response->set_chosen_option(options.on_confirm
                                            ? options.on_confirm(event.confirmation_request())
                                            : std::string());
            stream->Write(answer);
        }

        bool done = false;
        if (!half_closed) {
            // CommandResult is the terminator here, the way TurnCompletedEvent
            // is for a turn: a confirming command emits at least one other event
            // before it, so anything less specific would half-close too early.
            done = !on_event(event) || event.has_command_result();
        }
        if (done && !half_closed) {
            stream->WritesDone();
            half_closed = true;
            // Keep reading, for the same reason RunTurn does: half-closing
            // says we are done SENDING, and abandoning the read side would
            // wedge the server's writer mid-Write().
        }
    }
    if (!half_closed) stream->WritesDone();
    return stream->Finish();
}

grpc::Status SessionClient::Attach(const std::string& session_id,
                                   const std::function<bool(const v1::ServerEvent&)>& on_event) {
    grpc::ClientContext context;
    auto stream = stub_->OpenSession(&context);

    // The attach payload names the stream and nothing more: the session on the
    // other end runs its own work (a subagent's single turn), and this client
    // is here for its events.
    v1::ClientEvent attach;
    attach.set_session_id(session_id);
    attach.mutable_attach();
    if (!stream->Write(attach)) {
        stream->WritesDone();
        return stream->Finish();
    }

    v1::ServerEvent event;
    bool half_closed = false;
    while (stream->Read(&event)) {
        if (!half_closed && !on_event(event)) {
            // Consumer is done (a subagent's turn_completed is the ordinary
            // signal). Half-close, then keep reading to end of stream --
            // abandoning the read side would wedge the server's writer.
            stream->WritesDone();
            half_closed = true;
        }
    }
    if (!half_closed) stream->WritesDone();
    return stream->Finish();
}

// ---------------------------------------------------------------------------
// Stream (persistent per-session bidi + reader/writer threads).
// Lifecycle is deliberately two-phase: Open()/Attach() create the session and queue
// the naming event but DO NOT start threads; Resume() launches them only after the
// owning AppState has registered the tab. This prevents a dispatched event from ever
// racing an entry that does not exist yet (which would otherwise need-lock on an
// uninitialised TabEntry).
// ---------------------------------------------------------------------------

SessionClient::Stream::Stream(std::shared_ptr<grpc::Channel> ch, EventCallback cb)
    : channel_(std::move(ch)),
      stub_(v1::SessionService::NewStub(channel_)),
      event_cb_(std::move(cb)) {}

SessionClient::Stream::~Stream() { Close(); }

std::optional<std::string> SessionClient::Stream::Open(const CreateOptions& opts) {
    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_project_root(opts.project_root);
    request.set_agent_preset(opts.agent_preset);
    request.set_name(opts.name);
    request.set_run_mode(opts.run_mode);

    v1::CreateSessionResponse response;
    SetStatus(stub_->CreateSession(&context, request, &response));
    if (!status().ok()) {
        return std::nullopt;
    }
    session_id_ = response.session().session_id();

    // Name the stream on an empty ClientEvent carrying just the id — server attaches the
    // bidi to this session without firing a turn/command. Sent FIRST (FIFO queue) before
    // any user turn, so attach is complete even if the app queues its first payload fast.
    EnqueueNamingEvent();
    OpenBidi();   // create the bidi (lazy); threads start later
    return session_id_;
}

std::optional<std::string> SessionClient::Stream::Attach(const std::string& sid) {
    session_id_ = sid;
    EnqueueNamingEvent();   // same side-effect-free naming for attach/resume
    OpenBidi();
    return session_id_;
}

void SessionClient::Stream::Resume() {
    if (resumed_.exchange(true)) return;   // start reader/writer at most once
    LaunchReaderThread();
    LaunchWriterThread();
}

void SessionClient::Stream::SetOnClosed(std::function<void(const grpc::Status&)> cb) {
    on_closed_ = std::move(cb);   // set before Resume(); the reader thread reads it once
}

bool SessionClient::Stream::Send(const v1::ClientEvent& event) {
    // closing_ is re-checked UNDER qmtx_: Close() sets it under the same lock,
    // so an event can no longer slip into the queue after the writer has
    // already observed an empty queue and exited (the lost-event race).
    {
        std::lock_guard lk(qmtx_);
        if (closing_.load()) return false;
        if (send_queue_.size() >= kSendQueueCap) return false;   // stalled stream: refuse
        send_queue_.push_back(event);
    }
    send_cv_.notify_one();
    return true;
}

void SessionClient::Stream::Close() {
    if (closed_.exchange(true)) return;     // idempotent latch: only first caller proceeds
    {
        std::lock_guard lk(qmtx_);
        closing_ = true;                    // under qmtx_: see Send()
    }
    send_cv_.notify_all();                  // wake writer so it can exit

    // Cancel BEFORE the joins. A writer blocked in Write() (server stopped
    // reading) or a reader blocked in Read() (wedged connection) would hang
    // these joins forever -- and Close() runs on the UI thread from
    // CloseTab/quit. Cancel costs queued-but-unsent events, which is
    // consistent with Close's destructiveness: the session is being torn down
    // regardless.
    context_.TryCancel();

    if (writer_.joinable()) writer_.join();
    // Reader keeps reading until the server finishes (it will, once our
    // half-close or the cancel arrives). Read-to-EOF first -- the -p client's
    // hard-won lesson: Finish() before the reader drains deadlocks both sides.
    // The reader thread itself Finish()es the call when Read() returns false.
    if (reader_.joinable()) {
        reader_.join();
    } else if (bidi_) {
        // Threads never started (Open/Attach failed, or an abandoned
        // duplicate): nothing can race us; reap the call directly.
        SetStatus(bidi_->Finish());
    }
}

void SessionClient::Stream::Cancel() {
    context_.TryCancel();
}

grpc::Status SessionClient::Stream::status() const {
    std::lock_guard lk(status_mtx_);
    return status_;
}

void SessionClient::Stream::SetStatus(const grpc::Status& status) {
    std::lock_guard lk(status_mtx_);
    status_ = status;
}

void SessionClient::Stream::EnqueueNamingEvent() {
    v1::ClientEvent ev;
    ev.set_session_id(session_id_);   // the ONLY field set: names the stream, fires no action
    std::unique_lock lk(qmtx_);
    send_queue_.push_back(ev);
}

void SessionClient::Stream::OpenBidi() {
    // context_ is a member (not a local): gRPC requires it to outlive the call,
    // and this call outlives OpenBidi() by design -- Read()/Write() happen from
    // the reader/writer threads long after this function returns.
    //
    // Returns void, deliberately: OpenSession() only constructs the call
    // (gRPC connects lazily), so there is no status to check here -- a
    // connection failure surfaces later, as the reader's Finish() status, not
    // as a false "open succeeded". The first draft returned an unconditional
    // OK and the callers' !ok() checks were dead.
    bidi_ = stub_->OpenSession(&context_);   // concrete type: grpc::ClientReaderWriter<Client,Server>
}

void SessionClient::Stream::LaunchReaderThread() {
    reader_ = std::thread([this] {
        v1::ServerEvent event;
        while (bidi_ && bidi_->Read(&event)) {
            if (event_cb_) event_cb_(event);   // delivers to AppState; may itself Send()
        }
        // Read() returned false: the server closed its send side -- because the
        // session terminated, because our own half-close or Close()'s cancel
        // led it to finish, or because the TRANSPORT FAILED (server died).
        // This thread drained the call, so it owns Finish() -- whose status is
        // the only way a transport death is distinguishable from a clean close.
        if (bidi_) SetStatus(bidi_->Finish());
        // The status is captured BEFORE the callback: the callback's last
        // act can release the last shared_ptr holding this Stream, and ~Stream
        // (running right here on the reader thread) must not race it.
        if (on_closed_) {
            const grpc::Status final_status = status();
            on_closed_(final_status);
        }
    });
}

void SessionClient::Stream::LaunchWriterThread() {
    writer_ = std::thread([this] {
        while (true) {
            v1::ClientEvent ev;
            bool enqueued = false;
            {
                std::unique_lock lk(qmtx_);
                send_cv_.wait_for(lk, kWriterWakeup, [this] {
                    return closing_.load() || !send_queue_.empty();
                });
                if (send_queue_.empty()) {
                    // Either a timeout with nothing queued (wait_for's predicate can
                    // return false on timeout, which this loop must not treat as "an
                    // event is ready" -- front()/pop_front() on an empty deque is UB),
                    // or closing_ with the queue already drained.
                    if (closing_.load()) break;
                    continue;
                }
                ev = std::move(send_queue_.front());
                send_queue_.pop_front();
                enqueued = true;
            }
            if (!enqueued) continue;   // wakeup with no event — re-check closing_ next loop

            // Blocks freely here (writer thread), but never on the reader: gRPC permits one
            // concurrent Read + one Write per bidi. A blocked Write() means the server stopped
            // reading — a protocol bug worth surfacing loudly (see ARCHITECTURE.md Concurrency).
            if (!bidi_->Write(ev)) break;   // write error — let Close() drain/Finish safely
        }
        // Half-close our send side now that we have stopped writing. The reader continues
        // consuming the server's final output until Read()==false, then Finish().
        if (bidi_) bidi_->WritesDone();
    });
}

}  // namespace minimal::client
