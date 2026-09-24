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
#include <cstdint>
#include <functional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_launch.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/commands/project_command_source.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/rpc/auth_interceptor.hpp"
#include "minimal/rpc/command_executor.hpp"
#include "minimal/rpc/event_broadcaster.hpp"
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
    // Registries read from `registries`, all optional:
    //   commands            -- null: every command invocation answers "not
    //                          available" rather than being dropped.
    //   auth_providers      -- null: the pass-through default (no
    //                          authentication), matching the local-only topology.
    //   mention_resolvers   -- null: SuggestPaths uses the core-default
    //                          PrefixMentionResolver.
    //   services            -- the sealed PluginCall/PluginSubscribe dispatch
    //                          target; null: both RPCs answer UNAVAILABLE.
    //   permission_advisors -- null: permissions.json's advisor never applies.
    SessionServiceImpl(agent::SessionRegistry& sessions, session::SessionDatabase& db,
                       config::SettingsConfig settings, config::PermissionsConfig permissions,
                       common::Logger& logger, const plugins::HostRegistries& registries = {});

    grpc::Status CreateSession(grpc::ServerContext* context,
                               const v1::CreateSessionRequest* request,
                               v1::CreateSessionResponse* response) override;

    grpc::Status ForkSession(grpc::ServerContext* context,
                             const v1::ForkSessionRequest* request,
                             v1::ForkSessionResponse* response) override;

    grpc::Status OpenSession(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<v1::ServerEvent, v1::ClientEvent>* stream) override;

    grpc::Status GetMessages(grpc::ServerContext*, const v1::GetMessagesRequest*,
                             v1::GetMessagesResponse*) override;
    grpc::Status GetSession(grpc::ServerContext*, const v1::GetSessionRequest*,
                            v1::SessionSummary*) override;
    grpc::Status ListSessions(grpc::ServerContext*, const v1::ListSessionsRequest*,
                              v1::ListSessionsResponse*) override;
    grpc::Status ListAgentPresets(grpc::ServerContext*, const v1::ListAgentPresetsRequest*,
                                  v1::ListAgentPresetsResponse*) override;
    grpc::Status ListModels(grpc::ServerContext*, const v1::ListModelsRequest*,
                            v1::ListModelsResponse*) override;
    grpc::Status ListMcpServers(grpc::ServerContext*, const v1::ListMcpServersRequest*,
                                v1::ListMcpServersResponse*) override;
    grpc::Status SetMcpServerEnabled(grpc::ServerContext*, const v1::SetMcpServerEnabledRequest*,
                                     v1::SetMcpServerEnabledResponse*) override;
    grpc::Status SuggestPaths(grpc::ServerContext*, const v1::SuggestPathsRequest*,
                              v1::SuggestPathsResponse*) override;
    grpc::Status ListCommands(grpc::ServerContext*, const v1::ListCommandsRequest*,
                              v1::ListCommandsResponse*) override;
    grpc::Status RunSessionlessCommand(grpc::ServerContext*,
                                       const v1::RunSessionlessCommandRequest*,
                                       v1::RunSessionlessCommandResponse*) override;
    grpc::Status DeleteSession(grpc::ServerContext*, const v1::DeleteSessionRequest*,
                               v1::DeleteSessionResponse*) override;
    grpc::Status Ping(grpc::ServerContext*, const v1::PingRequest*,
                      v1::PingResponse*) override;
    grpc::Status PluginCall(grpc::ServerContext*, const v1::PluginCallRequest*,
                            v1::PluginCallResponse*) override;
    grpc::Status PluginSubscribe(grpc::ServerContext*, const v1::PluginCallRequest*,
                                 grpc::ServerWriter<v1::PluginCallResponse>*) override;

    // Invoked once the attached-client count returns to zero after having been
    // non-zero. THE SERVER IS REFCOUNTED BY ATTACHED CLIENTS AND EXITS WHEN THE
    // LAST ONE LEAVES -- that is what removes the daemon requirement, and what a
    // later auto-spawning client will rely on.
    void set_on_last_client_detached(std::function<void()> callback);

    // Optional: the project-scoped half of the catalog (ProjectCommandSource),
    // consulted BEFORE commands_ by both DispatchCommand's reader-thread
    // pre-check and ListCommands -- see FindEffective's own doc for why both
    // must agree. Null means every project-command lookup degrades to
    // global-only, exactly the pre-existing behavior.
    void set_project_commands(const commands::ProjectCommandSource* project_commands) {
        project_commands_ = project_commands;
    }
    void set_mcp_manager(mcp::McpClientManager* manager) { mcp_manager_ = manager; }

    // Optional: null means ListAgentPresets answers with an empty list rather
    // than dropping the RPC -- a picker that gets nothing back reads as "no
    // presets configured", which is at least honest, versus a hang.
    void set_presets(const agent::AgentPresetRegistry* presets) { presets_ = presets; }

    // Optional: null means signals are not offered -- a CreateSessionRequest
    // naming any signal answers INVALID_ARGUMENT rather than silently
    // subscribing to nothing. Set before serving (main.cpp, after the router
    // exists). The router is also how signal channels reach their sessions.
    void set_signal_router(agent::SignalRouter* router) { signal_router_ = router; }

    std::size_t attached_clients() const;

    // How long since a live client last Pinged (see PingRequest); duration::
    // max() when none ever has. main wires this into the reaper's idle-exit
    // half so a session-less-but-alive TUI does not read as "nobody here".
    std::chrono::milliseconds client_idle_for() const;

private:
    // settings.memory.scrollback_page_size, or the built-in floor when it is
    // unset. Shared by GetMessages and ListSessions -- both page, both need a
    // bound, and one default is easier to document than two.
    int ScrollbackPageSize() const;

    // Reader thread. Every branch is non-blocking -- that is the property the
    // three-thread topology depends on, not an implementation detail.
    void Dispatch(const v1::ClientEvent& event, agent::Session& session);

    // Read-only commands go to the bounded command pool, everything else to the
    // session's work queue. Reader thread; must not block on either path.
    void DispatchCommand(const v1::CommandInvocation& invocation, agent::Session& session);

    // S5.3: broadcasts PromptResolvedEvent(request_id) to every subscriber
    // once Resolve() reports THIS answer as the winner -- called from
    // Dispatch, reader thread, so this must not block (Publish never does).
    void PublishPromptResolved(agent::Session& session, const std::string& request_id);

    // What agent::Launch/Fork borrow from this service.
    agent::LaunchContext MakeLaunchContext();

    // Validates CreateSessionRequest.signals against the router and stages the
    // session-side consequences onto params: the wake flag (a subscribed
    // session exists to react, so it wakes for injections), the
    // channel-instruction context, and the router pointer + selector list
    // themselves. Actual subscription registration happens later, inside
    // SessionRegistry::CreateResolved -- atomically with the session becoming
    // findable, not from this RPC layer -- so this function only STAGES the
    // decision, it never calls Subscribe. Shared by fresh creation and resume
    // -- subscriptions never persist across a stream, so a resumed session
    // gets whatever THIS request declares. No router, or an empty request:
    // OK, no subscriptions, no wake change.
    grpc::Status ApplySignals(const v1::CreateSessionRequest& request,
                              agent::SessionParams& params) const;


    // The one decrement of attached_, shared by every exit -- the successful
    // detach, the attach-refused exits (which counted themselves first, see
    // OpenSession), and the unwinding exits. Firing the last-client callback
    // is part of the decrement, not something callers may do selectively: a
    // zero the callback never observed is exactly the leak this split once was.
    void ReleaseClientSlot();

    // RAII for one attached_ client slot: the count leads the claim (see
    // OpenSession's comment), and the DECREMENT must be as unconditional as
    // the increment. A bare fetch_add left the fallible stretch between it
    // and the finish lambda unprotected -- an exception from Subscribe() or
    // EmitSessionMetadata() unwound past every restore and leaked the slot,
    // after which the refcounted process would never believe it has zero
    // clients. Every scope that holds a slot constructs this; every exit,
    // exception or not, runs the decrement.
    class AttachedClientSlot {
    public:
        explicit AttachedClientSlot(SessionServiceImpl* self) : self_(self) {
            self_->attached_.fetch_add(1, std::memory_order_acq_rel);
        }
        AttachedClientSlot(const AttachedClientSlot&) = delete;
        AttachedClientSlot& operator=(const AttachedClientSlot&) = delete;
        ~AttachedClientSlot() { self_->ReleaseClientSlot(); }

    private:
        SessionServiceImpl* self_;
    };

    // The metadata event a freshly attached stream opens with: name,
    // agent_preset, model, permission_mode and the resolved context window --
    // the wire source for the status bar's {model}/{agent}/{permission_mode}
    // and {tokens} used/max. One emission per attach, targeted at THAT
    // subscriber (the streams already attached have theirs), before the
    // client's first event is dispatched, so a tab has everything it renders
    // from the first frame.
    void EmitSessionMetadata(agent::Session& session,
                             const rpc::EventBroadcaster::Handle& subscriber);

    // Runs read-only commands off the reader thread. Its destructor joins the
    // pool, and a task may borrow `sessions_`/`commands_` -- both outlive this
    // service (main declares them first), so a task can never dangle.
    CommandExecutor command_executor_{/*threads=*/4, /*max_backlog=*/64};

    agent::SessionRegistry& sessions_;
    // Borrowed and optional: a service built without commands answers every
    // invocation with "not available" rather than dropping it.
    const commands::CommandRegistry* commands_ = nullptr;
    const commands::ProjectCommandSource* project_commands_ = nullptr;
    mcp::McpClientManager* mcp_manager_ = nullptr;
    // Borrowed and optional: null = ListAgentPresets answers empty (see setter).
    const agent::AgentPresetRegistry* presets_ = nullptr;
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
    // Steady-clock milliseconds at the most recent Ping; -1 = none ever.
    // Atomic because the reaper thread reads it between heartbeats. Kept as
    // raw steady ms (not a time_point member read under a lock) so the
    // reaper's poll stays lock-free on both sides.
    std::atomic<std::int64_t> last_ping_steady_ms_{-1};
};

}  // namespace minimal::rpc
