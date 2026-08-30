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
    request.set_project_root(options.project_root);
    request.set_agent_preset(options.agent_preset);
    request.set_name(options.name);
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

grpc::Status SessionClient::Attach(
    const std::string& session_id,
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
