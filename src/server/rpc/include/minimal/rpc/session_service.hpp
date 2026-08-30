#pragma once

// The gRPC surface. Implements CreateSession and OpenSession; every other RPC
// answers UNIMPLEMENTED until the subsystem behind it exists (scrollback and
// ListSessions need SQLite storage, SuggestPaths needs the TUI's resolver,
// PluginCall needs client plugins). Answering explicitly matters: a client that
// calls one gets a clear status instead of a hang.
//
// THREE THREADS PER SESSION, and this class owns two of the boundaries:
//
//   Reader -- the OpenSession handler thread itself. EVERY dispatch branch below
//             is non-blocking. It reads, dispatches, and goes straight back to
//             reading, because it is the only thread that can deliver the answer
//             a blocked worker is waiting for.
//   Writer -- one per attached stream, started here, and the SOLE caller of
//             stream->Write(). gRPC permits one concurrent reader and one
//             concurrent writer but not concurrent writes, and three independent
//             sources produce outbound events.
//
// See ARCHITECTURE.md "Concurrency model".

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "minimal/agent/session_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/v1/session_service.grpc.pb.h"

namespace minimal::rpc {

class SessionServiceImpl final : public v1::SessionService::Service {
public:
    SessionServiceImpl(agent::SessionRegistry& sessions, config::SettingsConfig settings,
                       config::PermissionsConfig permissions, common::Logger& logger);

    grpc::Status CreateSession(grpc::ServerContext* context,
                               const v1::CreateSessionRequest* request,
                               v1::CreateSessionResponse* response) override;

    grpc::Status OpenSession(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<v1::ServerEvent, v1::ClientEvent>* stream) override;

    grpc::Status GetMessages(grpc::ServerContext*, const v1::GetMessagesRequest*,
                             v1::GetMessagesResponse*) override;
    grpc::Status ListSessions(grpc::ServerContext*, const v1::ListSessionsRequest*,
                              v1::ListSessionsResponse*) override;
    grpc::Status SuggestPaths(grpc::ServerContext*, const v1::SuggestPathsRequest*,
                              v1::SuggestPathsResponse*) override;
    grpc::Status PluginCall(grpc::ServerContext*, const v1::PluginCallRequest*,
                            v1::PluginCallResponse*) override;
    grpc::Status PluginSubscribe(grpc::ServerContext*, const v1::PluginCallRequest*,
                                 grpc::ServerWriter<v1::PluginCallResponse>*) override;

    // Invoked once the attached-client count returns to zero after having been
    // non-zero. THE SERVER IS REFCOUNTED BY ATTACHED CLIENTS AND EXITS WHEN THE
    // LAST ONE LEAVES -- that is what removes the daemon requirement, and what a
    // later auto-spawning client will rely on.
    void set_on_last_client_detached(std::function<void()> callback);

    std::size_t attached_clients() const;

private:
    // Reader thread. Returns false if the stream should end.
    bool Dispatch(const v1::ClientEvent& event, agent::Session& session);

    agent::SessionRegistry& sessions_;
    config::SettingsConfig settings_;
    config::PermissionsConfig permissions_;
    common::Logger& logger_;

    std::function<void()> on_last_client_detached_;
    std::atomic<std::size_t> attached_{0};
    std::atomic<std::uint64_t> next_session_id_{0};
};

}  // namespace minimal::rpc
