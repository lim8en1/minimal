#include "minimal/rpc/session_service.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "minimal/rpc/proto_convert.hpp"

namespace minimal::rpc {
namespace {

std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The first line of a message, capped. A placeholder until a SessionObserver
// replaces it with an LLM-generated title -- which is exactly the flow
// SessionMetadataEvent exists to carry.
std::string DefaultSessionName(const std::string& requested) {
    if (!requested.empty()) return requested;
    return "session";
}

}  // namespace

SessionServiceImpl::SessionServiceImpl(agent::SessionRegistry& sessions,
                                       config::SettingsConfig settings,
                                       config::PermissionsConfig permissions,
                                       common::Logger& logger)
    : sessions_(sessions),
      settings_(std::move(settings)),
      permissions_(std::move(permissions)),
      logger_(logger) {}

void SessionServiceImpl::set_on_last_client_detached(std::function<void()> callback) {
    on_last_client_detached_ = std::move(callback);
}

std::size_t SessionServiceImpl::attached_clients() const {
    return attached_.load(std::memory_order_acquire);
}

grpc::Status SessionServiceImpl::CreateSession(grpc::ServerContext*,
                                               const v1::CreateSessionRequest* request,
                                               v1::CreateSessionResponse* response) {
    agent::SessionParams params;
    params.session_id =
        "s-" + std::to_string(NowUnixMs()) + "-" +
        std::to_string(next_session_id_.fetch_add(1, std::memory_order_relaxed));
    params.name = DefaultSessionName(request->name());
    params.agent_preset =
        request->agent_preset().empty() ? settings_.default_agent_preset : request->agent_preset();
    params.project_root = request->project_root();

    // UNSPECIFIED means "the client did not ask", which is why FromProto reports
    // nullopt rather than a default: only here is the config available to
    // resolve it, and collapsing the two earlier would silently override a
    // deliberate client choice with permissions.json.
    params.run_mode_override = FromProto(request->run_mode());

    if (params.agent_preset.empty()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "no agent_preset given and settings.json sets no "
                            "default_agent_preset");
    }
    if (params.project_root.empty()) {
        // project_root is per-session and client-supplied: it is the boundary for
        // path confinement and bash's initial cwd, so defaulting it server-side
        // would silently point the agent at the server's cwd.
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "project_root is required");
    }

    std::shared_ptr<agent::Session> session;
    try {
        session = sessions_.Create(params);
    } catch (const std::exception& e) {
        // Preset and provider problems are configuration errors the operator has
        // to see and fix, so they come back with their reason attached rather
        // than as a generic INTERNAL.
        logger_.warn("CreateSession failed: {}", e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }

    auto* summary = response->mutable_session();
    summary->set_session_id(session->info().session_id);
    summary->set_name(session->info().name);
    summary->set_agent_preset(session->info().agent_preset);
    summary->set_project_root(session->info().project_root.string());
    summary->set_created_at_unix_ms(NowUnixMs());
    summary->set_updated_at_unix_ms(NowUnixMs());
    return grpc::Status::OK;
}

bool SessionServiceImpl::Dispatch(const v1::ClientEvent& event, agent::Session& session) {
    switch (event.payload_case()) {
        case v1::ClientEvent::kChatTurn:
            // Queues and returns. Running the turn here would block the reader,
            // which is the deadlock this whole topology exists to avoid.
            session.SubmitTurn(event.chat_turn().text());
            return true;

        case v1::ClientEvent::kPermissionResponse:
            // Carried across as bytes: parsing belongs on the worker, which can
            // afford to fail, not on the reader, which must not stall.
            session.pending().Resolve(event.permission_response().request_id(),
                                      event.permission_response().SerializeAsString());
            return true;

        case v1::ClientEvent::kConfirmationResponse:
            session.pending().Resolve(event.confirmation_response().request_id(),
                                      event.confirmation_response().chosen_option());
            return true;

        case v1::ClientEvent::kCancel:
            session.Cancel();
            return true;

        case v1::ClientEvent::kCommand: {
            // The command subsystem is not built. Answering explicitly beats
            // silence: a client waiting on a CommandResult would otherwise hang
            // on a request the server quietly dropped.
            v1::ServerEvent reply;
            reply.set_session_id(session.info().session_id);
            auto* result = reply.mutable_command_result();
            result->set_request_id(event.command().request_id());
            result->set_status(v1::COMMAND_STATUS_ERROR);
            result->set_message("the /-command subsystem is not implemented yet");
            session.queue().Push(std::move(reply));
            return true;
        }

        case v1::ClientEvent::PAYLOAD_NOT_SET:
            logger_.warn("session {}: client event with no payload", session.info().session_id);
            return true;
    }
    return true;
}

grpc::Status SessionServiceImpl::OpenSession(
    grpc::ServerContext*, grpc::ServerReaderWriter<v1::ServerEvent, v1::ClientEvent>* stream) {
    // OpenSession carries no request message: the stream is named by the
    // session_id on its first ClientEvent (deliberately redundant on every
    // message rather than bound once via metadata). So attach happens on the
    // first read, and the initial-attach guard covers the window until then.
    v1::ClientEvent first;
    if (!stream->Read(&first)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "stream closed before naming a session");
    }

    auto session = sessions_.Find(first.session_id());
    if (session == nullptr) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "unknown session_id '" + first.session_id() + "'");
    }

    session->MarkAttached();
    attached_.fetch_add(1, std::memory_order_acq_rel);

    // The one and only Write() caller for this stream. It drains the SESSION's
    // queue, not a stream-owned one, so anything produced before this moment is
    // still here and goes out first.
    std::thread writer([stream, session] {
        while (auto event = session->queue().Pop()) {
            if (!stream->Write(*event)) break;  // client went away
        }
    });

    Dispatch(first, *session);

    v1::ClientEvent event;
    while (stream->Read(&event)) {
        if (!Dispatch(event, *session)) break;
    }

    // Stream close TERMINATES the session -- there is no detached state in v1,
    // so a dropped stream is unambiguous. Terminate() closes the queue, which is
    // what lets the writer finish rather than blocking forever on Pop().
    sessions_.Terminate(session->info().session_id);
    session->queue().Close();
    if (writer.joinable()) writer.join();

    if (attached_.fetch_sub(1, std::memory_order_acq_rel) == 1 && on_last_client_detached_) {
        on_last_client_detached_();
    }
    return grpc::Status::OK;
}

namespace {
grpc::Status NotYet(const char* what) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, std::string(what) + " is not implemented yet");
}
}  // namespace

grpc::Status SessionServiceImpl::GetMessages(grpc::ServerContext*, const v1::GetMessagesRequest*,
                                             v1::GetMessagesResponse*) {
    return NotYet("GetMessages (needs SQLite session storage)");
}

grpc::Status SessionServiceImpl::ListSessions(grpc::ServerContext*, const v1::ListSessionsRequest*,
                                              v1::ListSessionsResponse*) {
    return NotYet("ListSessions (needs SQLite session storage)");
}

grpc::Status SessionServiceImpl::SuggestPaths(grpc::ServerContext*, const v1::SuggestPathsRequest*,
                                              v1::SuggestPathsResponse*) {
    return NotYet("SuggestPaths");
}

grpc::Status SessionServiceImpl::PluginCall(grpc::ServerContext*, const v1::PluginCallRequest*,
                                            v1::PluginCallResponse*) {
    return NotYet("PluginCall");
}

grpc::Status SessionServiceImpl::PluginSubscribe(grpc::ServerContext*,
                                                 const v1::PluginCallRequest*,
                                                 grpc::ServerWriter<v1::PluginCallResponse>*) {
    return NotYet("PluginSubscribe");
}

}  // namespace minimal::rpc
