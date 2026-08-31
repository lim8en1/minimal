#include "minimal/client/session_client.hpp"

#include <chrono>
#include <utility>

#include <grpcpp/create_channel.h>

namespace minimal::client {

SessionClient::SessionClient(std::string server_address)
    : server_address_(std::move(server_address)),
      channel_(grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials())),
      stub_(v1::SessionService::NewStub(channel_)) {}

SessionClient::~SessionClient() = default;

bool SessionClient::WaitForConnected(std::chrono::milliseconds timeout) {
    return channel_->WaitForConnected(std::chrono::system_clock::now() + timeout);
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

grpc::Status SessionClient::RunTurn(
    const std::string& session_id, const std::string& text,
    const std::function<bool(const v1::ServerEvent&)>& on_event) {
    grpc::ClientContext context;
    auto stream = stub_->OpenSession(&context);

    // The first ClientEvent is what names the session server-side and attaches
    // this stream to it -- OpenSession carries no request message of its own.
    v1::ClientEvent turn;
    turn.set_session_id(session_id);
    turn.mutable_chat_turn()->set_text(text);
    if (!stream->Write(turn)) {
        stream->WritesDone();
        return stream->Finish();
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
            stream->WritesDone();
            half_closed = true;
            // Keep reading. Half-closing says we are done SENDING; it does not
            // entitle us to stop receiving, and abandoning the read side would
            // wedge the server's writer mid-Write().
        }
    }
    if (!half_closed) stream->WritesDone();
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
            // Keep reading, for the same reason RunTurn does: half-closing says
            // we are done SENDING, and abandoning the read side wedges the
            // server's writer mid-Write().
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

}  // namespace minimal::client
