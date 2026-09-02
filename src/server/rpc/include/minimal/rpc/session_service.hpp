#pragma once

// The gRPC surface. Implements CreateSession, OpenSession, GetMessages,
// ListSessions, SuggestPaths, ListCommands, DeleteSession and the generic
// plugin channel (PluginCall/PluginSubscribe dispatch through the sealed
// ServiceRegistry). Answering explicitly matters: a client that calls
// anything gets a clear status instead of a hang.
//
// The two read RPCs go to the DATABASE, not to a live Session, and that is the
// point: the picker lists sessions the server has no Session object for, and
// scrollback must read the same way whether or not a tab happens to be open.
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

#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/rpc/auth_interceptor.hpp"
#include "minimal/rpc/command_executor.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/v1/session_service.grpc.pb.h"

namespace minimal::mcp {
class McpClientManager;
}  // namespace minimal::mcp

namespace minimal::agent {
class SignalRouter;  // signal subscriptions + delivery (borrowed via setter)
}  // namespace minimal::agent

namespace minimal::plugins {
class ServiceRegistry;  // the generic plugin channel's dispatch target (E5)
}  // namespace minimal::plugins

namespace minimal::rpc {

class SessionServiceImpl final : public v1::SessionService::Service {
public:
    SessionServiceImpl(agent::SessionRegistry& sessions, session::SessionDatabase& db,
                       config::SettingsConfig settings, config::PermissionsConfig permissions,
                       common::Logger& logger,
                       const tools::PermissionAdvisorRegistry* permission_advisors = nullptr);

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
    grpc::Status ListCommands(grpc::ServerContext*, const v1::ListCommandsRequest*,
                              v1::ListCommandsResponse*) override;
    grpc::Status DeleteSession(grpc::ServerContext*, const v1::DeleteSessionRequest*,
                               v1::DeleteSessionResponse*) override;
    grpc::Status PluginCall(grpc::ServerContext*, const v1::PluginCallRequest*,
                            v1::PluginCallResponse*) override;
    grpc::Status PluginSubscribe(grpc::ServerContext*, const v1::PluginCallRequest*,
                                 grpc::ServerWriter<v1::PluginCallResponse>*) override;

    // Invoked once the attached-client count returns to zero after having been
    // non-zero. THE SERVER IS REFCOUNTED BY ATTACHED CLIENTS AND EXITS WHEN THE
    // LAST ONE LEAVES -- that is what removes the daemon requirement, and what a
    // later auto-spawning client will rely on.
    void set_on_last_client_detached(std::function<void()> callback);

    // Optional: a service without one answers every invocation with "not
    // available" rather than dropping it. Set before serving.
    void set_commands(const commands::CommandRegistry* commands) { commands_ = commands; }
    void set_mcp_manager(mcp::McpClientManager* manager) { mcp_manager_ = manager; }

    // Optional: null means signals are not offered -- a CreateSessionRequest
    // naming any signal answers INVALID_ARGUMENT rather than silently
    // subscribing to nothing. Set before serving (main.cpp, after the router
    // exists). The router is also how signal channels reach their sessions.
    void set_signal_router(agent::SignalRouter* router) { signal_router_ = router; }

    // Optional; absent means the pass-through default (no authentication), which
    // matches the local-only topology. Set before serving.
    void set_auth_providers(const AuthProviderRegistry* registry) {
        auth_providers_ = registry;
    }

    // Optional: unset, SuggestPaths uses the core-default PrefixMentionResolver;
    // set (main.cpp after plugin load), a plugin's override takes precedence.
    void set_mention_resolvers(const agent::MentionResolverRegistry* resolvers) {
        mention_resolvers_ = resolvers;
    }

    // The PluginCall/PluginSubscribe dispatch target: the sealed ServiceRegistry
    // plugins provided their "<plugin_name>.rpc" handlers (or a PluginEndpoint,
    // see docs/EXTENSIBILITY.md 2.5) into at load, sealed before serving (which
    // is what makes unguarded Find from these handler threads safe). Borrowed
    // and optional; null means both RPCs answer UNAVAILABLE rather than
    // silently empty.
    void set_services(const plugins::ServiceRegistry* services) { services_ = services; }

    std::size_t attached_clients() const;

private:
    // settings.memory.scrollback_page_size, or the built-in floor when it is
    // unset. Shared by GetMessages and ListSessions -- both page, both need a
    // bound, and one default is easier to document than two.
    int ScrollbackPageSize() const;

    // CreateSession with session_id set: reopen a stored session rather than
    // creating a new one. The stored row's name/agent_preset/project_root
    // govern -- the request may not carry its own -- and a failure NEVER
    // deletes the row, unlike fresh creation's rollback of its just-minted one.
    grpc::Status ResumeSession(const v1::CreateSessionRequest* request,
                               v1::CreateSessionResponse* response);

    // Reader thread. Every branch is non-blocking -- that is the property the
    // three-thread topology depends on, not an implementation detail.
    void Dispatch(const v1::ClientEvent& event, agent::Session& session);

    // Read-only commands go to the bounded command pool, everything else to the
    // session's work queue. Reader thread; must not block on either path.
    void DispatchCommand(const v1::CommandInvocation& invocation, agent::Session& session);

    // Parses CreateSessionRequest.model onto params. Shared by fresh creation
    // and resume, so the two cannot disagree about whether --model applies.
    grpc::Status ApplyModelOverride(const v1::CreateSessionRequest& request,
                                    agent::SessionParams& params) const;

    // Validates CreateSessionRequest.signals against the router and stages the
    // session-side consequences onto params: the wake flag (a subscribed
    // session exists to react, so it wakes for injections) and the
    // channel-instruction context. Shared by fresh creation and resume --
    // subscriptions never persist across a stream, so a resumed session gets
    // whatever THIS request declares. No router, or an empty request: OK, no
    // subscriptions, no wake change.
    grpc::Status ApplySignals(const v1::CreateSessionRequest& request,
                              agent::SessionParams& params) const;

    // Registers the session with the router under its declared selectors.
    // Runs AFTER the session exists (subscriptions hold a weak_ptr to it);
    // the caller then hands the same session to the stream. Validated already
    // by ApplySignals, so a failure here is logged, not surfaced: the session
    // exists either way.
    void SubscribeSignals(std::shared_ptr<agent::Session> session,
                          const v1::CreateSessionRequest& request) const;

    // Detach and drop this stream's reference, firing on_last_client_detached_
    // if it was the last. BOTH exits of OpenSession go through here: the catch
    // path used to decrement without firing, which left the refcounted server
    // unable to ever notice it had zero clients.
    void ReleaseAttachment(agent::Session& session);

    // The one decrement of attached_, shared by every exit -- the successful
    // detach, the attach-refused exits (which counted themselves first, see
    // OpenSession), and the unwinding exits. Firing the last-client callback
    // is part of the decrement, not something callers may do selectively: a
    // zero the callback never observed is exactly the leak this split once was.
    void ReleaseClientSlot();

    // The metadata event a freshly attached stream opens with: name,
    // agent_preset, model, permission_mode and the resolved context window --
    // the wire source for the status bar's {model}/{agent}/{permission_mode}
    // and {tokens} used/max. One emission per attach, before the client's
    // first event is dispatched, so a tab has everything it renders from the
    // first frame.
    void EmitSessionMetadata(agent::Session& session);

    // Runs read-only commands off the reader thread. Its destructor joins the
    // pool, and a task may borrow `sessions_`/`commands_` -- both outlive this
    // service (main declares them first), so a task can never dangle.
    CommandExecutor command_executor_{/*threads=*/4, /*max_backlog=*/64};

    agent::SessionRegistry& sessions_;
    // Borrowed and optional: a service built without commands answers every
    // invocation with "not available" rather than dropping it.
    const commands::CommandRegistry* commands_ = nullptr;
    mcp::McpClientManager* mcp_manager_ = nullptr;
    // Borrowed and optional: absent = pass-through (see auth_provider.hpp).
    const AuthProviderRegistry* auth_providers_ = nullptr;
    // Borrowed and optional: null falls back to the core default resolver.
    const agent::MentionResolverRegistry* mention_resolvers_ = nullptr;
    // Borrowed and optional: null = the generic plugin channel is not offered.
    const plugins::ServiceRegistry* services_ = nullptr;
    // Borrowed and optional: null = signals are not offered (see setter).
    agent::SignalRouter* signal_router_ = nullptr;
    session::SessionDatabase& db_;
    session::SessionReader reader_;
    config::SettingsConfig settings_;
    // Borrowed and optional: absent = no advisor can be selected, every Ask
    // reaches the human. Selected per session in the params producers below.
    const tools::PermissionAdvisorRegistry* permission_advisors_ = nullptr;
    config::PermissionsConfig permissions_;
    common::Logger& logger_;

    std::function<void()> on_last_client_detached_;
    std::atomic<std::size_t> attached_{0};
};

}  // namespace minimal::rpc
