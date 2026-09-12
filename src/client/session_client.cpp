#include "minimal/client/session_client.hpp"

#include <chrono>
#include <utility>

#include <grpcpp/create_channel.h>
#include <grpcpp/support/channel_arguments.h>

namespace minimal::client {

namespace {
// How long the writer waits for a new event before re-checking closing_. Bounds how
// long Close() takes once triggered.
inline constexpr std::chrono::milliseconds kWriterWakeup = std::chrono::milliseconds(50);
// @-mention suggestions run on the frame loop while the user types -- a wedged
// server must cost the editor a bounded pause, not the UI (this was the one
// unary call with no deadline; every other caller bounds its fetches).
inline constexpr std::chrono::milliseconds kSuggestPathsDeadline = std::chrono::milliseconds(250);
// How long Detach() waits for its writer to flush the DETACH before issuing
// the context cancel as a last resort (see Stream::Detach): long enough for
// any healthy round trip, far short of hanging quit forever.
inline constexpr std::chrono::seconds kDetachJoinTimeout = std::chrono::seconds(2);

// Symmetric with the server's explicit limit (server.cpp): the gRPC default
// capped RECEIVE at 4 MB -- a large tool result streamed into the client's
// GetMessages/attach path failed with an opaque RESOURCE_EXHAUSTED -- while
// leaving send unlimited. Both directions pinned at 64 MiB, matching the
// tool-output cap every producer applies.
std::shared_ptr<grpc::Channel> MakeChannel(const std::string& target) {
    grpc::ChannelArguments args;
    constexpr int kMaxMessageBytes = 64 * 1024 * 1024;
    args.SetMaxReceiveMessageSize(kMaxMessageBytes);
    args.SetMaxSendMessageSize(kMaxMessageBytes);
    return grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args);
}
}  // namespace

// ---------------------------------------------------------------------------
// SessionClient (shared channel + stub for headless single-turn plumbing)
// ---------------------------------------------------------------------------

SessionClient::SessionClient(std::string server_address)
    : server_address_(std::move(server_address)),
      channel_(MakeChannel(server_address_)),
      stub_(v1::SessionService::NewStub(channel_)) {}

SessionClient::~SessionClient() {
    // Cancel any live PluginSubscribe readers so their Read()s return instead
    // of racing this destruction. The registry is shared with the subscribers
    // (see SubscribeRegistry in the header): each subscriber holds its own
    // reference, so a subscriber woken by this cancel can still run its
    // erase AFTER this destructor body -- against a registry that is still
    // alive. (The previous plain-member version raced that erase against
    // the member set's own destruction: use-after-destroy.)
    CancelSubscriptions();
}

void SessionClient::CancelSubscriptions() {
    std::lock_guard lk(subscribes_->mtx);
    // Latched, not just a one-time sweep -- see SubscribeRegistry::shutting_down.
    subscribes_->shutting_down = true;
    for (auto* context : subscribes_->active) context->TryCancel();
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
    // Deadline like every other unary call: a wedged (or still-spawning)
    // server must surface as DEADLINE_EXCEEDED, not hang headless startup
    // forever. Generous -- session creation touches the DB -- but bounded.
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
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
    for (const auto& selector : options.signals) {
        request.add_signals(selector);
    }

    v1::CreateSessionResponse response;
    const grpc::Status status = stub_->CreateSession(&context, request, &response);
    return CreateResult{response.session().session_id(), status};
}

SessionClient::GetMessagesResult SessionClient::GetMessages(const std::string& session_id,
                                                             std::optional<int64_t> before_seq,
                                                             int32_t limit) {
    grpc::ClientContext context;
    // Deadline like the other unary calls: an unbounded GetMessages on a
    // wedged server blocked headless startup indefinitely.
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
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

SessionClient::ListSessionsResult SessionClient::ListSessions(const ListSessionsOptions& options) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::ListSessionsRequest request;
    request.set_project_root(options.project_root);
    request.set_include_all_dirs(options.include_all_dirs);
    request.set_include_subagents(options.include_subagents);
    request.set_limit(options.limit);
    if (options.before_updated_at.has_value()) request.set_before_updated_at(*options.before_updated_at);
    if (options.before_id.has_value()) request.set_before_id(*options.before_id);

    ListSessionsResult result;
    result.status = stub_->ListSessions(&context, request, &result.response);
    return result;
}

SessionClient::GetSessionResult SessionClient::GetSession(const std::string& session_id) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::GetSessionRequest request;
    request.set_session_id(session_id);

    GetSessionResult result;
    result.status = stub_->GetSession(&context, request, &result.summary);
    return result;
}

// Presence heartbeat (see PingRequest in the proto). Fire-and-forget: the
// caller (the TUI's heartbeat thread) treats every failure -- including "the
// server is gone" -- as a non-event; the next beat simply tries again until
// the process exits. A short deadline so a wedged connection cannot park the
// thread for the default gRPC deadline.
grpc::Status SessionClient::Ping() {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
    v1::PingRequest request;
    v1::PingResponse response;
    return stub_->Ping(&context, request, &response);
}

v1::CommandResult SessionClient::RunSessionlessCommand(const std::string& name,
                                                       const std::vector<std::string>& positional,
                                                       const std::string& raw) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::RunSessionlessCommandRequest request;
    request.set_name(name);
    for (const auto& value : positional) request.mutable_args()->add_positional(value);
    request.mutable_args()->set_raw(raw);
    v1::RunSessionlessCommandResponse response;
    const grpc::Status status = stub_->RunSessionlessCommand(&context, request, &response);
    if (!status.ok()) {
        // The caller surfaces the result on a notice line; a transport-level
        // refusal must arrive as the same shape, never as a lost answer.
        v1::CommandResult failed;
        failed.set_status(v1::COMMAND_STATUS_ERROR);
        failed.set_message(status.error_message());
        return failed;
    }
    return response.result();
}

SessionClient::ListCommandsResult SessionClient::ListCommands(const std::string& project_root) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::ListCommandsRequest request;
    request.set_project_root(project_root);
    ListCommandsResult result;
    result.status = stub_->ListCommands(&context, request, &result.response);
    return result;
}

SessionClient::ListAgentPresetsResult SessionClient::ListAgentPresets(
    const std::string& project_root) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::ListAgentPresetsRequest request;
    request.set_project_root(project_root);
    ListAgentPresetsResult result;
    result.status = stub_->ListAgentPresets(&context, request, &result.response);
    return result;
}

SessionClient::ListModelsResult SessionClient::ListModels(const ListModelsOptions& options) {
    grpc::ClientContext context;
    // A refresh waits on every discover-entry's catalog fetch (parallel
    // server-side, bounded by the slowest provider's own timeout); a bare
    // listing touches only config. The 15 s covers the refresh path's worst
    // case without letting a hung provider wedge the picker indefinitely.
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(
                              options.refresh_discovery ? 15 : 2));
    ListModelsResult result;
    v1::ListModelsRequest request;
    request.set_refresh_discovery(options.refresh_discovery);
    result.status = stub_->ListModels(&context, request, &result.response);
    return result;
}

SessionClient::ListMcpServersResult SessionClient::ListMcpServers() {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::ListMcpServersRequest request;
    ListMcpServersResult result;
    result.status = stub_->ListMcpServers(&context, request, &result.response);
    return result;
}

SessionClient::SetMcpServerEnabledResult SessionClient::SetMcpServerEnabled(const std::string& name,
                                                                            bool enabled) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    v1::SetMcpServerEnabledRequest request;
    request.set_name(name);
    request.set_enabled(enabled);
    SetMcpServerEnabledResult result;
    result.status = stub_->SetMcpServerEnabled(&context, request, &result.response);
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
    // OUR OWN channel + stub, not `this->channel_`/`this->stub_`: this call
    // can run on a plugin's own thread for the plugin's whole lifetime (no
    // deadline, see below), and ~SessionClient's CancelSubscriptions only
    // REQUESTS cancellation -- it does not wait for this call to actually
    // finish. A subscriber still inside Read()/Finish() when SessionClient
    // (and its channel_/stub_ members) is destroyed would otherwise
    // dereference a freed grpc::Channel through the completion queue. A
    // local shared_ptr copy keeps the channel alive for exactly as long as
    // THIS call needs it, independent of SessionClient's own lifetime -- the
    // same "OUR reference, not this->" rule `registry` below already
    // follows.
    const auto channel = channel_;
    const auto stub = v1::SessionService::NewStub(channel);
    // No deadline: a subscription lives as long as the subscriber does. The
    // escape hatch is on_payload returning false, which cancels the call.
    // Registered for teardown: a subscriber still blocked in Read() at client
    // destruction is unblocked by ~SessionClient's cancel sweep instead of
    // racing destruction. `registry` is OUR reference to the shared registry
    // (not `this->subscribes_` re-read each time): it keeps the mutex and set
    // alive even if ~SessionClient finishes while this subscriber is still
    // winding down -- every access below touches the shared_ptr's object,
    // never a member that could already be destroyed.
    const auto registry = subscribes_;
    {
        std::lock_guard lk(registry->mtx);
        // Re-checked under the lock, same pattern Stream::Send() uses
        // against closing_: shutting_down can only flip to true while we
        // hold this lock, so seeing it false here means CancelSubscriptions
        // cannot have swept (or ever will sweep) this context -- refuse
        // instead of registering a context nothing will ever cancel.
        if (registry->shutting_down) return SubscribeOutcome::kCancelled;
        registry->active.insert(&context);
    }
    v1::PluginCallRequest request;
    request.set_plugin_name(plugin_name);
    request.set_method(method);
    request.set_json_payload(json_payload);
    auto reader = stub->PluginSubscribe(&context, request);
    v1::PluginCallResponse response;
    while (reader->Read(&response)) {
        if (on_payload && !on_payload(response.json_payload())) {
            context.TryCancel();
            {
                std::lock_guard lk(registry->mtx);
                registry->active.erase(&context);
            }
            return SubscribeOutcome::kCancelled;
        }
    }
    const grpc::Status final_status = reader->Finish();
    {
        std::lock_guard lk(registry->mtx);
        registry->active.erase(&context);
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
    const std::function<void(const std::shared_ptr<TurnHandle>&)>& on_handle,
    const TurnOverrides& overrides) {
    grpc::ClientContext context;
    auto stream = stub_->OpenSession(&context);

    // The first ClientEvent is what names the session server-side and attaches this
    // stream to it -- OpenSession carries no request message of its own.
    v1::ClientEvent turn;
    turn.set_session_id(session_id);
    auto* chat = turn.mutable_chat_turn();
    chat->set_text(text);
    // Per-turn overrides ride as raw refs: splitting and resolution happen
    // server-side at turn pop (a malformed/unknown ref fails the TURN with a
    // named error, never a silent fall-back).
    if (!overrides.model.empty()) chat->set_model(overrides.model);
    if (!overrides.agent_preset.empty()) chat->set_agent_preset(overrides.agent_preset);
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
        // Deferred-cancel handoff: a Cancel published by on_handle (above)
        // ran before the stream was stored and latched cancel_pending
        // instead of no-op'ing. Deliver it now, BEFORE the first Read(), so
        // the interrupt cannot be lost to the publication window.
        if (control->cancel_pending && !stream->Write(control->cancel_event)) {
            minimal::common::DefaultLogger().warn(
                "deferred turn cancel could not be written (stream already dead)");
        }
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
        if (!half_closed && event.has_ask_request()) {
            // A model question has no headless answer. Unlike the permission
            // case above this is EXPECTED here (ask_user is ReadOnly, so no
            // run mode gates it), and the decline is a real outcome: the
            // broker maps answered=false onto "the user did not answer", a
            // tool result the model adapts to. Never hang waiting for input
            // that cannot come, and never invent a selection -- bypass's
            // blind first-option answer is for destructive-command
            // confirmations, not for putting opinions in the user's mouth.
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_ask_response();
            response->set_request_id(event.ask_request().request_id());
            response->set_answered(false);
            std::lock_guard<std::mutex> lock(control->mu);
            stream->Write(answer);
        }
        bool done = false;
        if (!half_closed) {
            // TurnCompletedEvent is the ONLY reliable end-of-turn signal. A
            // tool-using turn emits several message_deltas before it is actually
            // finished, so stop_reason alone would half-close mid-turn.
            // SessionEndedEvent is the OTHER terminal: the session was killed
            // server-side, so no turn_completed will ever follow.
            done = !on_event(event) || event.has_turn_completed() || event.has_session_ended();
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
            // SessionEndedEvent terminals this stream too -- a killed session
            // will never produce the CommandResult.
            done = !on_event(event) || event.has_command_result() || event.has_session_ended();
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
        if (!half_closed && event.has_ask_request()) {
            // Same headless rule as RunTurn's: a model question has no
            // headless answer, and a subagent asking on its own stream would
            // block this whole run's teardown on input that cannot come.
            // answered=false is a real outcome the model adapts to ("the
            // user did not answer"), never an invented selection.
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_ask_response();
            response->set_request_id(event.ask_request().request_id());
            response->set_answered(false);
            stream->Write(answer);
        }
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
    // Bounded like every other unary call here: the TUI calls Open on the loop
    // thread (first-run create, typing on an empty app), and a hung server
    // must not hang the frame loop past this.
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    v1::CreateSessionRequest request;
    if (!opts.resume_session_id.empty()) {
        // RESUME of a stored session: the stored row governs name, agent_preset
        // and project_root, and the server REJECTS a resume carrying any of
        // them (SessionServiceImpl::ResumeSession) -- only the id (and the
        // client-owned run_mode) go out. Mirrors the headless --resume path;
        // the TUI's picker resume rides the same branch. Skipping this step
        // and only naming the bidi was the "picked a past session, then
        // typing did nothing" bug: OpenSession resolves its id against the
        // LIVE registry only, answered NOT_FOUND, and the dead stream
        // swallowed every prompt.
        request.set_session_id(opts.resume_session_id);
        request.set_run_mode(opts.run_mode);
    } else {
        request.set_project_root(opts.project_root);
        request.set_agent_preset(opts.agent_preset);
        request.set_name(opts.name);
        request.set_run_mode(opts.run_mode);
        // Sent either way -- see CreateOptions::model/signals. A /model choice
        // staged on a sessionless tab rides these; dropping them here silently
        // discarded the staged selection.
        request.set_model(opts.model);
        for (const auto& selector : opts.signals) {
            request.add_signals(selector);
        }
    }

    v1::CreateSessionResponse response;
    SetStatus(stub_->CreateSession(&context, request, &response));
    if (!status().ok()) {
        return std::nullopt;
    }
    session_id_ = response.session().session_id();

    // Name the stream with an Attach{} payload (see EnqueueNamingEvent):
    // server attaches the bidi to this session without firing a turn/command.
    // Sent FIRST (FIFO queue) before any user turn, so attach is complete even
    // if the app queues its first payload fast.
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
    std::lock_guard<std::mutex> lk(launch_mtx_);
    // Idle -> Running at most once: covers a second Resume() call (the
    // existing rule) AND a Resume() racing a Close()/Detach() that already
    // closed this stream before it ever started (see launch_mtx_'s comment
    // in the header) -- in the latter case the threads below must never
    // launch at all.
    if (launch_state_ != LaunchState::kIdle) return;
    launch_state_ = LaunchState::kRunning;
    ever_resumed_ = true;
    // Launched INSIDE the lock: the thread constructors return immediately
    // (nothing in them can block on launch_mtx_), so this can't deadlock --
    // and it closes the narrower race where Close()/Detach() could otherwise
    // observe kRunning before writer_/reader_ are actually assigned.
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
    {
        std::lock_guard<std::mutex> lk(launch_mtx_);
        if (launch_state_ == LaunchState::kClosed) return;   // idempotent latch
        launch_state_ = LaunchState::kClosed;   // a racing Resume() now sees this and no-ops
    }
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
        JoinReaderOrDetachSelf();
    } else if (bidi_) {
        // Threads never started (Open/Attach failed, or an abandoned
        // duplicate): nothing can race us; reap the call directly.
        SetStatus(bidi_->Finish());
    }
}

void SessionClient::Stream::JoinReaderOrDetachSelf() {
    if (!reader_.joinable()) return;
    if (reader_.get_id() == std::this_thread::get_id()) {
        // We ARE the reader thread: ~Stream is running here because the
        // on_closed_ callback dropped the last Stream reference. Joining
        // the current thread throws (and ~Stream would then std::terminate
        // on the still-joinable member it destroys), so detach instead.
        // Safe ONLY in this configuration -- the reader lambda's tail after
        // on_closed_ is empty (the self-join invariant on Close() in the
        // header), so this thread touches no member again before it
        // finishes -- and the detach keeps ~Stream's destruction of the
        // std::thread member legal.
        reader_.detach();
    } else {
        reader_.join();
    }
}

void SessionClient::Stream::Detach(EndSessionMode mode) {
    bool was_running;
    {
        std::lock_guard<std::mutex> lk(launch_mtx_);
        if (launch_state_ == LaunchState::kClosed) return;   // idempotent latch, shared with Close()
        was_running = (launch_state_ == LaunchState::kRunning);
        launch_state_ = LaunchState::kClosed;   // a racing Resume() now sees this and no-ops
    }

    if (!was_running) {
        // Resume() never ran, and per launch_state_ (kClosed now) never
        // will: there is no writer thread to flush the graceful EndSession
        // below, and the naming event this stream's Open()/Attach() queued
        // is still sitting unsent in send_queue_ -- the server is still
        // waiting to read it. The graceful path deliberately skips an
        // up-front cancel (see below); that assumption only holds once a
        // writer thread exists to actually deliver the EndSession. Cancel
        // and reap directly instead -- the same "abandoned before ever
        // attaching" handling AttachTab's duplicate-registration race
        // already uses (see AppState::AttachTab).
        context_.TryCancel();
        if (bidi_) SetStatus(bidi_->Finish());
        return;
    }

    v1::ClientEvent end_session;
    end_session.set_session_id(session_id_);
    end_session.mutable_end_session()->set_mode(mode == EndSessionMode::kTerminate
                                                    ? v1::EndSession::TERMINATE
                                                    : v1::EndSession::DETACH);
    {
        std::lock_guard lk(qmtx_);
        // Queued before closing_ flips, under the same lock Send() checks --
        // so this is never refused the way a Send() after Close() would be,
        // and the writer sees (and flushes) it before it observes closing_.
        send_queue_.push_back(std::move(end_session));
        closing_ = true;
    }
    send_cv_.notify_all();

    // No TryCancel up front: see the header. The writer flushes the
    // EndSession (and anything queued ahead of it) and calls WritesDone() on
    // its own; the server reads the mode, unsubscribes this stream (and
    // TERMINATEs the session, for TERMINATE), and returns -- ending the RPC
    // without needing to be cancelled.
    //
    // BOUNDED join, though: a server wedged mid-turn leaves the writer
    // blocked in a flow-control-stalled Write() where no amount of waiting
    // helps, and Detach runs at quit -- a plain join hung the whole client.
    // Past the bound the cancel IS issued (teardown; a cancelled status is
    // acceptable there, and any unsent tail is lost the same way Close()
    // would lose it). writer_exited_ -- latched under qmtx_ just before the
    // writer's final WritesDone -- is the timed wait's predicate; the
    // join() below only reaps an already-finishing thread.
    if (writer_.joinable()) {
        const auto deadline = std::chrono::steady_clock::now() + kDetachJoinTimeout;
        {
            std::unique_lock lk(qmtx_);
            if (!writer_cv_.wait_until(lk, deadline, [this] { return writer_exited_; })) {
                context_.TryCancel();
                // After the cancel a blocked Write() returns false, the
                // writer breaks out, and the flag lands; no further timeout
                // is needed (a second one could only paper over a genuine
                // gRPC bug worth surfacing as a hang).
                writer_cv_.wait(lk, [this] { return writer_exited_; });
            }
        }
        writer_.join();
    }
    if (reader_.joinable()) {
        // Bounded exactly like the writer's wait above, and for the same
        // reason Detach() runs at quit -- but the writer finishing (fast:
        // WritesDone() needs no reply) says nothing about whether the
        // SERVER is still alive. A server that stopped responding but never
        // closed the socket leaves the reader blocked in Read() forever;
        // the graceful path above deliberately skips an up-front cancel,
        // and without this bound that left quit hanging regardless of the
        // writer's own bound already firing. reader_exited_ -- latched
        // under qmtx_ right after the reader's own Finish() -- is the timed
        // wait's predicate.
        const auto deadline = std::chrono::steady_clock::now() + kDetachJoinTimeout;
        {
            std::unique_lock lk(qmtx_);
            if (!reader_cv_.wait_until(lk, deadline, [this] { return reader_exited_; })) {
                context_.TryCancel();
                reader_cv_.wait(lk, [this] { return reader_exited_; });
            }
        }
        JoinReaderOrDetachSelf();
    } else if (bidi_) {
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
    // The naming event MUST carry the empty Attach{} payload, not just the
    // id: a payload-less ClientEvent falls through the server's Dispatch to
    // its PAYLOAD_NOT_SET case, which logs "client event with no payload" on
    // every ordinary TUI open. Attach{} is the documented mechanism for
    // naming a stream without submitting work (session_service.proto), and
    // Dispatch's kAttach case is a deliberate no-op for an already-attached
    // stream -- idempotent, zero cost.
    v1::ClientEvent ev;
    ev.set_session_id(session_id_);
    ev.mutable_attach();
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
            received_any_.store(true, std::memory_order_release);
            if (event_cb_) {
                // Contained, not propagated: this delivers into AppState's
                // DispatchToTab (which may itself Send()), and an uncaught
                // throw here escapes the thread function -- std::terminate,
                // taking the whole client down (a std::thread constructor's
                // std::system_error under thread pressure, reachable from
                // AttachTab->Resume, is the concrete example, but any
                // throwing path in the dispatch chain works).
                try {
                    event_cb_(event);
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn(
                        "session {}: event callback threw: {}", session_id_, error.what());
                } catch (...) {
                    common::DefaultLogger().warn(
                        "session {}: event callback threw a non-standard exception",
                        session_id_);
                }
            }
        }
        // Read() returned false: the server closed its send side -- because the
        // session terminated, because our own half-close or Close()'s cancel
        // led it to finish, or because the TRANSPORT FAILED (server died).
        // This thread drained the call, so it owns Finish() -- whose status is
        // the only way a transport death is distinguishable from a clean close.
        if (bidi_) SetStatus(bidi_->Finish());
        // The stream is over either way -- latch closing_ BEFORE the callback:
        // a Send() accepted by a dead stream used to be dropped by the writer's
        // failed Write() with no error ever surfaced (the "picked a stored
        // session, then typing did nothing" bug). With the latch set, Send()
        // returns false and the AppState send paths surface it.
        {
            std::lock_guard lk(qmtx_);
            closing_.store(true, std::memory_order_release);
            reader_exited_ = true;   // Detach()'s bounded reader join predicate
        }
        send_cv_.notify_all();   // wake the writer onto the latch, not its poll timer
        reader_cv_.notify_all();   // wake Detach()'s bounded wait, not its timeout
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
        // Latch the exit for Detach()'s bounded join (see there): the wait
        // predicate needs a signal under qmtx_, since a thread blocked in
        // Write() gives a plain join() no timeout handle. Latched AFTER the
        // WritesDone -- once the flag is set, Detach may proceed to destroy
        // the stream, so nothing may touch `this` below it.
        {
            std::lock_guard lk(qmtx_);
            writer_exited_ = true;
        }
        writer_cv_.notify_all();
    });
}

}  // namespace minimal::client
