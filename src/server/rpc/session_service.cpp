#include "minimal/rpc/session_service.hpp"

#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <set>
#include <thread>
#include <utility>

#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/signal_router.hpp"
#include "minimal/commands/command_dispatcher.hpp"
#include "minimal/config/permission_codec.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/mcp/signal_protocol.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/rpc/event_translator.hpp"
#include "minimal/rpc/proto_convert.hpp"

#include "minimal/plugins/plugin_rpc.hpp"

namespace minimal::rpc {
namespace {

// The floor under both paged RPCs when settings.json leaves scrollback_page_size
// unset, which is its default. Named rather than repeated as a literal: two
// call sites drifting apart is how a "default" becomes two different defaults.
constexpr int kFallbackPageSize = 100;

// CEILING on a client-supplied page size, and it is load-bearing rather than
// tidy. `limit` arrives as a wire int32 with a floor and nothing above it, and
// SessionDatabase binds `limit + 1` to reserve the has-more probe: INT32_MAX is
// signed-overflow UB, and in practice binds a NEGATIVE limit, which SQLite reads
// as no limit at all. One GetMessages{limit: 2147483647} then loads an entire
// transcript -- every row, every JSON decode, every proto copy, in one response
// -- which is precisely the O(n) blowup the keyset paging exists to prevent.
// ListSessions' proto comment already documented a bound it did not have.
constexpr int kMaxPageSize = 1000;

// Clamped, never rejected: a client asking for more than we will serve is not
// making an error, and paging is the caller's answer either way. `has_more`
// already tells it there is another page.
int ClampPageSize(int requested, int fallback) {
    if (requested <= 0) requested = fallback;
    return std::min(requested, kMaxPageSize);
}

// The fallback when no registry was wired (tests, or a service built before
// plugin load completes). Stateless and const, so one shared instance is safe.
const agent::MentionResolver& DefaultMentionResolver() {
    static const agent::PrefixMentionResolver resolver;
    return resolver;
}

// Session's public surface deliberately never names rpc::EventTranslator --
// this is the one cast back to the concrete type, for the transport
// operations (Subscribe/Unsubscribe/Pop/Publish/PublishTo/Resolve) that only
// this layer needs. Safe: `session` owns the referenced EventTranslator for
// as long as `session` itself is alive (SessionParams::transport_handle is
// required at construction, never reassigned after).
EventTranslator& Transport(agent::Session& session) {
    return *static_cast<EventTranslator*>(session.transport().get());
}

}  // namespace

SessionServiceImpl::SessionServiceImpl(agent::SessionRegistry& sessions,
                                       session::SessionDatabase& db,
                                       config::SettingsConfig settings,
                                       config::PermissionsConfig permissions,
                                       common::Logger& logger,
                                       const plugins::HostRegistries& registries)
    : sessions_(sessions),
      db_(db),
      reader_(db),
      settings_(std::move(settings)),
      permission_advisors_(registries.permission_advisors),
      permissions_(std::move(permissions)),
      logger_(logger) {
    commands_ = registries.commands;
    auth_providers_ = registries.auth_providers;
    mention_resolvers_ = registries.mention_resolvers;
    services_ = registries.services;
}

namespace {

// Looks up permissions.json's configured advisor (if any) and records the
// SELECTION on params -- composition happens later, in
// rpc::FinishSessionWiring, called from the one funnel every session (this
// path and SubagentManager's advisor-inheritance path alike) passes through.
// No advisor configured, or the named plugin not loaded (a runtime condition
// the sanity check warns about): params carry no selection and
// FinishSessionWiring builds the wire default -- the human, as always.
void ApplyAdvisor(const config::PermissionsConfig& permissions,
                  const tools::PermissionAdvisorRegistry* advisors, agent::SessionParams& params) {
    if (!permissions.advisor || advisors == nullptr) return;
    params.selected_advisor = advisors->Find(*permissions.advisor);
    params.advisor_timeout = std::chrono::milliseconds(permissions.advisor_timeout_ms);
}

}  // namespace

void SessionServiceImpl::set_on_last_client_detached(std::function<void()> callback) {
    on_last_client_detached_ = std::move(callback);
}

std::size_t SessionServiceImpl::attached_clients() const {
    return attached_.load(std::memory_order_acquire);
}

std::chrono::milliseconds SessionServiceImpl::client_idle_for() const {
    const std::int64_t last = last_ping_steady_ms_.load(std::memory_order_acquire);
    if (last < 0) {
        // Nobody ever pinged: as "quiet for as long as the reaper can see".
        return std::chrono::milliseconds::max();
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return std::chrono::milliseconds(std::max<std::int64_t>(0, now_ms - last));
}

// Client presence heartbeat (see PingRequest in the proto). A live client
// with nothing attached -- the TUI before its first session -- sends these so
// the reaper's idle exit can tell "nobody is here" from "somebody is here
// with nothing to show for it yet". Touch nothing but the timestamp.
grpc::Status SessionServiceImpl::Ping(grpc::ServerContext* context, const v1::PingRequest*,
                                      v1::PingResponse*) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;
    last_ping_steady_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_release);
    return grpc::Status::OK;
}

int SessionServiceImpl::ScrollbackPageSize() const {
    const auto configured = static_cast<int>(settings_.memory.scrollback_page_size);
    return configured > 0 ? configured : kFallbackPageSize;
}

grpc::Status SessionServiceImpl::CreateSession(grpc::ServerContext* context,
                                               const v1::CreateSessionRequest* request,
                                               v1::CreateSessionResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    // session_id set means RESUME: reopen the stored session, loading its
    // transcript as the loop's working history. Distinct enough from creation
    // -- different validation, no row minting, no rollback-on-failure -- to be
    // its own path rather than a flag inside this one.
    if (!request->session_id().empty()) {
        return ResumeSession(request, response);
    }

    agent::SessionParams params;
    // Passed through EMPTY when the client sent nothing, never defaulted to a
    // placeholder. SessionDatabase names an unnamed session from its first user
    // message, and that rule is guarded by `name = ''` so it can never clobber a
    // name someone chose -- so a placeholder here would satisfy the guard and
    // permanently suppress the default naming. Every row in the picker would
    // read "session", and no test below the RPC layer would notice.
    params.name = request->name();
    params.agent_preset =
        request->agent_preset().empty() ? settings_.default_agent_preset : request->agent_preset();
    params.project_root = request->project_root();

    // UNSPECIFIED means "the client did not ask", which is why FromProto reports
    // nullopt rather than a default: this service deliberately holds no
    // permissions.json -- the run-mode default is applied where the rest of it
    // is, in AgentPresetRegistry (agent_preset_registry.cpp,
    // LoadPermissionsConfig). An earlier revision stored a PermissionsConfig
    // here and never read it, which made this comment read as if the default
    // were resolved on this path. Collapsing the wire value into a default
    // anywhere before the registry would silently override a deliberate client
    // choice with the file's.
    params.run_mode_override = FromProto(request->run_mode());
    params.mcp_manager = mcp_manager_;
    // A startup-config decision, resolved once: the session never changes its
    // answer mid-life, so a hot edit would not take effect anyway (config is
    // read once at startup).
    params.wake_on_injection = settings_.background_wake;
    params.pause_auto_resume_on_submit = settings_.pause_auto_resume_on_submit;

    if (const auto status = ApplyModelOverride(*request, params); !status.ok()) return status;

    if (params.agent_preset.empty()) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "no agent_preset given and settings.json sets no "
                            "default_agent_preset");
    }
    if (params.project_root.empty()) {
        // project_root is per-session and client-supplied: it is the boundary for
        // path confinement and bash's initial cwd, so defaulting it server-side
        // would silently point the agent at the server's own cwd.
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "project_root is required");
    }
    // project_root must exist and be writable: it is the confinement boundary
    // and bash's initial cwd, so a bad value should fail fast with a clear
    // reason rather than surface later as confusing tool errors.
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(params.project_root, ec) || ec) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "project_root is not an existing directory: " +
                                    params.project_root.string());
        }
        // faccessat with AT_EACCESS, not access(2): the check must answer for
        // the server's EFFECTIVE uid -- access(2) tests the real one, which
        // diverges under setuid/gid deployments and then lies in either
        // direction.
        if (::faccessat(AT_FDCWD, params.project_root.c_str(), W_OK, AT_EACCESS) != 0) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "project_root is not writable: " +
                                    params.project_root.string());
        }
    }

    // The DATABASE mints the id, so the row exists before the Session does. It
    // is the only party that can guarantee uniqueness across restarts, and the
    // transcript has to be attributable to a row from its very first append.
    session::SessionSummary row;
    try {
        row = db_.CreateSession(params.name, std::nullopt, params.project_root.string(),
                                params.agent_preset,
                                params.model_override
                                    ? std::optional<std::string>(params.model_override->str())
                                    : std::nullopt);
    } catch (const std::exception& e) {
        logger_.warn("CreateSession: could not record session: {}", e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    params.session_id = row.session_id;

    // After the id is minted: the wire checker inside the composition captures
    // the session_id, so the advisor cannot be applied earlier.
    ApplyAdvisor(permissions_, permission_advisors_, params);

    // Signals next: every selector is validated and every named channel
    // warmed BEFORE the session exists, so a bad or unreachable one refuses
    // the start (and rolls the just-minted row back below) instead of
    // delivering a session that silently never hears its channel.
    if (const auto status = ApplySignals(*request, params); !status.ok()) {
        try {
            db_.DeleteSession(row.session_id);
        } catch (const std::exception& e) {
            logger_.warn("CreateSession: rollback after signal refusal failed: {}", e.what());
        }
        return status;
    }

    std::shared_ptr<agent::Session> session;
    try {
        session = sessions_.Create(std::move(params));
    } catch (const std::exception& e) {
        // Preset and provider problems are configuration errors the operator has
        // to see and fix, so they come back with their reason attached rather
        // than as a generic INTERNAL.
        //
        // Roll the row back. Inserting first is what makes the id authoritative,
        // but it also means a failure here would otherwise leave a message-less
        // session sitting in the picker forever with no way to remove it.
        // Guarded: an exception leaving an RPC handler is UB (see
        // ResumeSession's comment), and a store failure here must not
        // shadow the FAILED_PRECONDITION the original error deserves.
        try {
            db_.DeleteSession(row.session_id);
        } catch (const std::exception& rollback_error) {
            logger_.warn("CreateSession: rollback after construction failure failed: {}",
                        rollback_error.what());
        }
        logger_.warn("CreateSession failed: {}", e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }

    *response->mutable_session() = ToProto(row);
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ForkSession(grpc::ServerContext* context,
                                             const v1::ForkSessionRequest* request,
                                             v1::ForkSessionResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (request->source_session_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "source_session_id is required");
    }

    // The model ref is shape-checked HERE, not at pop: this path can answer
    // INVALID_ARGUMENT directly, unlike a wire turn, so a malformed ref should
    // not survive to the queue.
    std::optional<config::ModelRef> model;
    if (!request->model().empty()) {
        model = config::SplitModelRef(request->model());
        if (!model) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "model must be \"<provider>/<model>\" (got \"" +
                                    request->model() + "\")");
        }
    }

    std::optional<std::string> agent_preset;
    if (!request->agent_preset().empty()) agent_preset = request->agent_preset();

    try {
        std::shared_ptr<agent::Session> session = sessions_.ForkSession(
            request->source_session_id(), request->target_seq(), request->name(), model,
            agent_preset, FromProto(request->run_mode()));
        // The registry returned the live session; read the persisted row back
        // for the summary (it carries message_count and fork provenance the
        // in-memory info does not).
        const auto row = db_.GetSession(session->info().session_id);
        if (!row) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "forked session row vanished");
        }
        *response->mutable_session() = ToProto(*row);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        // Store refusals (no such top-level session, invalid target) are caller
        // errors; preset/model resolution failures are configuration errors.
        // Both come back with their reason; the registry rolled the minted row
        // back on the latter already.
        logger_.warn("ForkSession failed: {}", e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }
}

grpc::Status SessionServiceImpl::ApplyModelOverride(const v1::CreateSessionRequest& request,
                                                    agent::SessionParams& params) const {    // Shape-checked here so a malformed reference is INVALID_ARGUMENT rather
    // than FAILED_PRECONDITION from deep inside session construction -- the
    // client sent something wrong, which is a different thing from the server
    // not being able to honour something well-formed.
    if (request.model().empty()) return grpc::Status::OK;
    // SplitModelRef, not ParseModelRef: a client may name a discovered model
    // whose id itself carries slashes ("openrouter/anthropic/claude-opus-5"),
    // and strict parsing would make those unaddressable from --model/resume.
    params.model_override = config::SplitModelRef(request.model());
    if (!params.model_override) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "model '" + request.model() +
                                "' must be <provider>/<model>, e.g. local/qwen4");
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ApplySignals(const v1::CreateSessionRequest& request,
                                              agent::SessionParams& params) const {
    if (request.signals().empty()) return grpc::Status::OK;
    if (signal_router_ == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "signals requested but this server does not offer them");
    }

    // Validate every selector BEFORE anything exists to roll back: a typo in
    // one of five selectors must refuse the session start, not quietly deliver
    // four of five signals. The channel halves are collected for the
    // connection warm-up below.
    std::vector<std::string> channels;
    for (const auto& selector : request.signals()) {
        std::string error;
        std::string channel;
        if (!signal_router_->CheckSelector(selector, error, &channel)) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error);
        }
        // The internal source has no connection to warm; every real channel
        // does. This is the lazy first start of a channel server -- the same
        // trigger an MCP tool server gets from a session whose preset names
        // it -- and a channel that fails to come up fails the session start:
        // subscribing to a channel that cannot connect would otherwise look
        // exactly like "connected but no signals ever arrive". A null manager
        // is a deliberately supported degraded mode (a host that wires the
        // router without a live MCP manager, e.g. a test harness): liveness
        // simply cannot be checked, so validation stays structural-only,
        // exactly like an internal-source selector.
        if (channel != mcp::kInternalSignalSource && mcp_manager_ != nullptr) {
            std::string connect_error;
            if (!mcp_manager_->EnsureAlive(channel, connect_error)) {
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                    "signal channel '" + channel + "' is not available: " +
                                        connect_error);
            }
        }
        if (std::find(channels.begin(), channels.end(), channel) == channels.end()) {
            channels.push_back(channel);
        }
    }

    // A subscribed session exists to react to signals: it wakes on injection.
    // The background_wake setting governs subagent completions and task
    // results; a subscription is the user's explicit, per-session request for
    // the same behavior from signals.
    params.wake_on_injection = true;
    params.signal_context = signal_router_->SignalContextFor(
        std::vector<std::string>(request.signals().begin(), request.signals().end()));
    // Staged for CreateResolved, not acted on here: subscription registration
    // happens INSIDE the registry's create funnel, atomically with the
    // session becoming findable, so no internal signal can ever observe this
    // session_id before its declared subscriptions exist. See
    // SessionRegistry::CreateResolved.
    params.signal_router = signal_router_;
    params.signal_selectors.assign(request.signals().begin(), request.signals().end());
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ResumeSession(const v1::CreateSessionRequest* request,
                                               v1::CreateSessionResponse* response) {
    // The stored row already carries name, agent_preset and project_root, and
    // it governs: a resumed session keeps the identity and confinement
    // boundary it was created with. Rejecting a conflicting field beats
    // silently ignoring it -- the client has all three from ListSessions, so
    // sending one is a bug worth naming. run_mode is the exception: it is a
    // property of the CLIENT, not of the session.
    if (!request->name().empty() || !request->agent_preset().empty() ||
        !request->project_root().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "resume carries session_id (and run_mode) only; the stored session "
                            "keeps its name, agent_preset, and project_root");
    }

    // GetSession throws on a store failure (locked database, I/O error,
    // corruption) rather than answering "no such session" -- and an exception
    // leaving an RPC handler is UB, so the store's throws are mapped here.
    std::optional<session::SessionSummary> stored;
    try {
        stored = db_.GetSession(request->session_id());
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!stored) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no such session: " + request->session_id());
    }
    // A subagent belongs to its parent's group and its lifecycle ended with
    // the parent's turn; resuming one standalone has no meaning in v1.
    if (stored->parent_session_id) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "subagent sessions cannot be resumed: " + request->session_id());
    }
    // Two live Sessions on one transcript would interleave appends, so an
    // already-open session is refused here rather than inside the registry.
    if (sessions_.Find(stored->session_id) != nullptr) {
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                            "session '" + stored->session_id + "' is already open");
    }

    agent::SessionParams params;
    params.session_id = stored->session_id;
    params.name = stored->name;
    params.agent_preset = stored->agent_preset;
    params.project_root = stored->project_root;
    params.run_mode_override = FromProto(request->run_mode());
    params.mcp_manager = mcp_manager_;
    params.resume = true;
    // Same startup-config decision as fresh creation: the setting is a
    // property of the deployment, not of the stored row.
    params.wake_on_injection = settings_.background_wake;
    params.pause_auto_resume_on_submit = settings_.pause_auto_resume_on_submit;
    ApplyAdvisor(permissions_, permission_advisors_, params);
    // APPLIES ON RESUME TOO, like run_mode and for the same reason: a model
    // named on THIS request outranks anything stored. Silently dropping it
    // would be exactly the failure the field checks above refuse to make for
    // name/preset/project_root.
    if (const auto status = ApplyModelOverride(*request, params); !status.ok()) return status;
    // No model on the request: restore the one the session was created (or
    // last /model-ed) with, persisted in the row (v8). Without this a resume
    // re-resolves the bare preset and its default model wins -- silently
    // undoing the user's earlier choice across a server restart. A request
    // model ("continue this conversation, but on the big model") takes
    // precedence by arriving through ApplyModelOverride above.
    if (!params.model_override && stored->model) {
        params.model_override = config::SplitModelRef(*stored->model);
    }
    // Same restore for the subagent-model choice (/subagent-model, v10):
    // without it a restart silently reverts spawned children to inheriting
    // the parent's model, undoing the user's earlier choice.
    if (stored->subagent_model) {
        params.subagent_model_override = config::SplitModelRef(*stored->subagent_model);
    }
    // Signals on resume, same rules as fresh creation: validated and warmed
    // before the session starts, never persisted from the previous stream.
    if (const auto status = ApplySignals(*request, params); !status.ok()) return status;

    // Read before params is moved from: a model the REQUEST named must outlive
    // the move, to be written into the row once the session exists.
    const auto request_model = params.model_override;

    std::shared_ptr<agent::Session> resumed;
    try {
        // Loads the stored transcript, repairs a torn tool-call tail if there
        // is one, and starts the session.
        resumed = sessions_.Create(std::move(params));
    } catch (const agent::SessionAlreadyOpen& e) {
        // The pre-check above refused with ALREADY_EXISTS for the session
        // being open, but the loser of a RACE (two resumes in flight) landed
        // here as FAILED_PRECONDITION -- the same condition, two statuses,
        // depending on timing. Match the pre-check.
        logger_.warn("resume of session {} lost the open race", stored->session_id);
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
    } catch (const std::exception& e) {
        // NO ROLLBACK here, unlike fresh creation: the row is the user's
        // existing session, not one this call just minted. A failure (the
        // stored preset no longer resolves, its provider kind no longer
        // registered) is reported with its reason and the transcript survives.
        logger_.warn("resume of session {} failed: {}", stored->session_id, e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }

    // The request's model outranked the stored one; make the row describe
    // what the session now runs, so the NEXT model-less resume keeps THIS
    // choice instead of silently restoring the superseded one. Written only
    // after construction succeeded (a failed resume must not rewrite the
    // row), best-effort like the runtime hook.
    if (request_model) {
        try {
            db_.UpdateSessionModel(stored->session_id, request_model->str());
        } catch (const std::exception& e) {
            logger_.warn("resume of session {}: persisting model override failed: {}",
                         stored->session_id, e.what());
        }
    }

    // Re-read rather than reuse `stored`: resume may have appended a repair
    // message, and the response should carry the current message_count.
    // Guarded and failure-mapped: a nullopt here would previously have been
    // dereferenced unchecked -- UB in the handler.
    std::optional<session::SessionSummary> fresh;
    try {
        fresh = db_.GetSession(stored->session_id);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!fresh) {
        // Unreachable while this process is the sole writer and just finished
        // constructing the session -- which is exactly why it must be a named
        // INTERNAL rather than a dereference of an empty optional.
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "session row vanished during resume: " + stored->session_id);
    }
    *response->mutable_session() = ToProto(*fresh);
    return grpc::Status::OK;
}

void SessionServiceImpl::PublishPromptResolved(agent::Session& session,
                                               const std::string& request_id) {
    v1::ServerEvent event;
    event.set_session_id(session.info().session_id);
    event.mutable_prompt_resolved()->set_request_id(request_id);
    Transport(session).queue().Publish(std::move(event));
}

void SessionServiceImpl::Dispatch(const v1::ClientEvent& event, agent::Session& session) {
    switch (event.payload_case()) {
        case v1::ClientEvent::kChatTurn: {
            // Queues and returns. Running the turn here would block the reader,
            // which is the deadlock this whole topology exists to avoid. The
            // raw ref strings ride as-is: splitting and resolution happen at
            // pop, so a malformed or unknown name fails the TURN with a named
            // error rather than being dropped reader-side.
            if (event.chat_turn().has_model() || event.chat_turn().has_agent_preset()) {
                agent::QueuedTurn turn;
                turn.text = event.chat_turn().text();
                turn.model = event.chat_turn().model();
                turn.agent_preset = event.chat_turn().agent_preset();
                session.SubmitTurnWithOverrides(std::move(turn));
            } else {
                session.SubmitTurn(event.chat_turn().text());
            }
            return;
        }

        case v1::ClientEvent::kPermissionResponse: {
            // Carried across as the typed variant, still unparsed: parsing
            // belongs on the worker, which can afford to fail, not on the
            // reader, which must not stall. The payload case IS the type --
            // the variant makes a wrong-kind answer structurally unreadable
            // at the waiter rather than merely failing to parse there.
            const auto& permission_response = event.permission_response();
            const auto& request_id = permission_response.request_id();
            PermissionAnswer answer;
            answer.decision = FromProto(permission_response.decision());
            answer.granted_keys.assign(permission_response.granted_keys().begin(),
                                       permission_response.granted_keys().end());
            answer.deny_message = permission_response.deny_message();
            answer.stop_turn = permission_response.stop_turn();
            if (Transport(session).pending().Resolve(request_id, std::move(answer))) {
                PublishPromptResolved(session, request_id);
            }
            return;
        }

        case v1::ClientEvent::kConfirmationResponse: {
            const auto& request_id = event.confirmation_response().request_id();
            if (Transport(session).pending().Resolve(
                    request_id, ConfirmationAnswer{event.confirmation_response().chosen_option()})) {
                PublishPromptResolved(session, request_id);
            }
            return;
        }

        case v1::ClientEvent::kAskResponse: {
            // Same shape as the confirmation case: the payload case IS the
            // type, Resolve is a no-op for an id with no live slot, and the
            // resolved broadcast dismisses the modal on every OTHER attached
            // client. The answer rides unvalidated -- shape filtering belongs
            // to the waiter (WireAskBroker), which holds the questions these
            // answers answer.
            const auto& ask_response = event.ask_response();
            const auto& request_id = ask_response.request_id();
            AskUserAnswer answer;
            answer.answered = ask_response.answered();
            for (const auto& wire_slot : ask_response.answers()) {
                AskUserAnswer::Slot slot;
                for (const auto& label : wire_slot.selected()) slot.selected.push_back(label);
                slot.other = wire_slot.other();
                answer.answers.push_back(std::move(slot));
            }
            if (Transport(session).pending().Resolve(request_id, std::move(answer))) {
                PublishPromptResolved(session, request_id);
            }
            return;
        }

        case v1::ClientEvent::kCancel:
            session.Cancel();
            // include_queued: the interrupt takes the queued messages with it
            // (messages submitted mid-turn and not yet started). Each dropped
            // turn announces itself; the in-flight turn unwinds as cancelled
            // either way.
            if (event.cancel().include_queued()) session.CancelAllQueuedTurns();
            return;

        case v1::ClientEvent::kCancelQueuedTurn:
            session.CancelQueuedTurn(event.cancel_queued_turn().turn_id());
            return;

        case v1::ClientEvent::kAttach:
            // Nothing to dispatch: naming the stream on its first event IS the
            // attach. This payload exists so a client can honour its attach
            // obligation toward a subagent child without submitting a turn to
            // a session that will never run one.
            return;

        case v1::ClientEvent::kCommand:
            DispatchCommand(event.command(), session);
            return;

        case v1::ClientEvent::kRewind: {
            // Queued for the worker like a mutating command: the rewind runs
            // at a turn boundary and refuses itself on the wire if the target
            // is not a rewoundable position. Nothing here blocks.
            //
            // The oneof is carried across explicitly: an UNSET action must
            // reach the session as "neither", not as the proto3 default that
            // `target_seq()` returns -- which would silently read as a rewind
            // to seq 0.
            agent::QueuedRewind queued;
            if (event.rewind().has_target_seq()) {
                queued.target_seq = event.rewind().target_seq();
            } else if (event.rewind().redo()) {
                queued.redo = true;
            }
            session.SubmitRewind(queued);
            return;
        }

        case v1::ClientEvent::kSetRunMode: {
            // Applied on the READER, like kSetPaused: the executor's run mode
            // is a single atomic enum read lock-free by every tool dispatch,
            // so an immediate flip is safe -- and a mid-turn press takes
            // effect at the very next gated tool call of the running turn,
            // instead of at the turn boundary a queued work item would wait
            // for. UNSPECIFIED carries no mode -- ignored with a warning
            // rather than silently flipping the session to the enum's
            // default. False (terminated session) needs no wire answer: the
            // stream is ending anyway.
            const auto mode = FromProto(event.set_run_mode().run_mode());
            if (!mode.has_value()) {
                logger_.warn("session {}: set_run_mode with no mode; ignored",
                             session.info().session_id);
                return;
            }
            session.SetRunMode(*mode);
            return;
        }

        case v1::ClientEvent::kSetPaused:
            // THE reader-thread control event, like Cancel: pause must be
            // able to land MID-TURN, and a mutating command would queue for
            // the worker and only apply at a turn boundary -- which is
            // precisely the delay pause exists to avoid. Session::SetPaused
            // flips an atomic, publishes metadata, and notifies the worker;
            // nothing here blocks.
            session.SetPaused(event.set_paused().paused());
            return;

        case v1::ClientEvent::kEndSession:
            // Never reached: OpenSession's read loop intercepts has_end_session()
            // before calling Dispatch, for both the first event and every
            // subsequent one, because ending the stream is a decision only
            // that loop can unwind around. Kept as an explicit case (rather
            // than falling to no case at all) so the switch stays exhaustive
            // and a future call site cannot reintroduce the silent-no-op bug
            // this once was.
            logger_.warn("session {}: end_session reached Dispatch (should be unreachable)",
                        session.info().session_id);
            return;

        case v1::ClientEvent::PAYLOAD_NOT_SET:
            logger_.warn("session {}: client event with no payload", session.info().session_id);
            return;

        default:
            // An UNKNOWN future payload case (a NEWER client against this
            // binary). The old exhaustive-but-closed switch let it fall
            // through silently -- backwards versus the unset case, which
            // warns: a warning on "nothing sent" but silence on "something
            // we cannot understand" is exactly wrong for wire evolution.
            // Warn-and-ignore keeps the server alive with an older schema;
            // the client should upgrade.
            logger_.warn("session {}: client event with unknown payload case {} (newer client?)",
                         session.info().session_id,
                         static_cast<int>(event.payload_case()));
            return;
    }
}

void SessionServiceImpl::DispatchCommand(const v1::CommandInvocation& invocation,
                                          agent::Session& session) {
    agent::QueuedCommand queued;
    queued.request_id = invocation.request_id();
    queued.name = invocation.name();
    queued.args = FromProto(invocation.args());

    // THE DISPATCH SPLIT (ARCHITECTURE.md, "/-command system"). Read-only means
    // "touches no session state", so it may answer while a turn is in flight --
    // but on a SHORT-LIVED TASK, never inline here: this is the reader thread,
    // and a command that blocked it would deadlock the very stream its answer
    // has to travel back down.
    //
    // Everything else queues for the worker, which pops only between turns. That
    // is what keeps a mutation from landing between a tool_use and its
    // tool_result -- a hard 400 on both wire formats.
    // FindEffective, not a bare commands_->Find: a command that exists ONLY
    // in this session's project registry must not read as "unknown" here and
    // then run fine through Session::ExecuteCommand's own Dispatch call --
    // both must agree, or the unknown-command refusal below fires before the
    // command ever gets a chance to run.
    const std::shared_ptr<const commands::CommandRegistry> project_holder =
        project_commands_ != nullptr ? project_commands_->For(session.info().project_root)
                                     : nullptr;
    const commands::CommandRegistry* project_registry = project_holder.get();
    const commands::Command* command =
        commands_ != nullptr ? commands::FindEffective(*commands_, project_registry, queued.name)
                             : nullptr;
    // THE SUBAGENT GUARD, answered reader-side: a child's command surface is
    // read-only (see SessionRegistry::ApplyConstructorWiring for why the
    // registry is still wired -- /cost beside a live turn is fine, /agent's
    // preset re-resolution is not). Refusing HERE rather than queueing is the
    // whole point: a subagent's life is one turn, and the worker drain drops
    // everything queued after it finishes -- a mutating command typed on a
    // subagent tab mid-turn would otherwise vanish in silence. ReadOnly() is
    // the command object's own answer, so any future read-only command works
    // on a child for free and any mutating one is refused without a new
    // table. (Session::ExecuteCommand repeats the check as defense in depth
    // for dispatch paths that bypass this reader.)
    if (!session.info().parent_session_id.empty() && command != nullptr &&
        !command->ReadOnly()) {
        session.RejectCommand(queued, commands::SubagentRefusalMessage(queued.name));
        return;
    }
    // ReadOnlyFor, not ReadOnly: the invocation decides where it runs (a
    // bare /model opens a picker now, not after the turn); the subagent
    // guard above stays on the static answer.
    if (command != nullptr && command->ReadOnlyFor(queued.args)) {
        // On the bounded command pool, not a detached thread: a detached task
        // per command made a flood of read-only commands an unbounded pile of
        // never-joined threads and left a shutdown window where a still-running
        // one held shared_ptrs into registries being destroyed. The pool is
        // joined in this service's destructor, before those registries die.
        auto shared = sessions_.Find(session.info().session_id);
        if (shared != nullptr) {
            if (command_executor_.TryPost([shared, queued] { shared->RunCommandNow(queued); })) {
                return;
            }
            // Saturated: the caller holds a request_id a client waits on, so
            // refusal must still produce an answer.
            shared->RejectCommand(queued, "the command pool is saturated; retry shortly");
            return;
        }
        // The registry already erased this session_id -- SessionRegistry::
        // Terminate() erases the map entry before Session::Terminate() finishes
        // tearing the session down, so `session` (this call's own reference)
        // can still be a live, valid object here even though the lookup just
        // failed. Reject immediately rather than falling through into the
        // busy/unknown-command checks below, which have nothing useful to say
        // about a session that is no longer registered.
        session.RejectCommand(queued, "session is no longer registered");
        return;
    }

    // THE BUSY CHECK, which until now nothing ever performed: both callers of
    // Dispatch() passed `busy = false` unconditionally, so only the tests ever
    // exercised the refusal and `/compact` typed mid-turn was quietly queued --
    // exactly what command_dispatcher.hpp says must not happen ("queueing would
    // make /compact silently run minutes later against a context that had moved
    // on").
    //
    // AN LLMCommand, not merely a non-read-only one. The two are different
    // rules and both are real: a mutating HarnessCommand (`/agent`, `/model`)
    // SHOULD queue and run at the turn boundary -- that is what the shared work
    // queue is for -- while an LLMCommand needs the provider and the history to
    // itself and has nothing useful to do afterwards. The type exists precisely
    // to carry this distinction ("a type cannot be forgotten the way a flag
    // can"), so it is the type that is asked.
    //
    // Answered here rather than by passing `busy` down, because the answer has
    // to reach the client from the READER: queueing it for the worker is the
    // very thing being refused.
    if (!session.idle() && dynamic_cast<const commands::LLMCommand*>(command) != nullptr) {
        session.RejectCommand(queued, commands::BusyRefusalMessage(queued.name));
        return;
    }
    // An unknown command is answered now too. Queueing it to be told "unknown
    // command" after a minutes-long turn is a worse answer than the same answer
    // immediately, and nothing about the reply depends on the worker.
    //
    // Only when a registry EXISTS to be unknown to: with commands_ unset the
    // right answer is the session's "the /-command subsystem is not available",
    // which is a different fact and belongs where it already lives.
    if (commands_ != nullptr && command == nullptr) {
        session.RejectCommand(queued, commands::UnknownCommandMessage(queued.name));
        return;
    }

    session.SubmitCommand(std::move(queued));
}

grpc::Status SessionServiceImpl::OpenSession(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<v1::ServerEvent, v1::ClientEvent>* stream) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

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

    // THE COUNT LEADS THE CLAIM. attached_ used to be incremented only AFTER
    // the attach succeeded, which opened a window between the two: this stream
    // was provably attached but not yet counted, so another stream's
    // ReleaseClientSlot could fetch_sub from 1 to 0 -- firing
    // on_last_client_detached_, i.e. requesting process shutdown, while a
    // client was attached. Counted here via RAII (and released on every exit
    // path below, including the unwind paths): the count is accurate from the
    // moment the stream is committed to serving this session, and no throw
    // between here and `finish` can leak it anymore.
    const AttachedClientSlot client_slot(this);
    // A session the reaper or another stream is already tearing down must not be
    // attached to at all: its broadcaster is closing, and the client would see an
    // immediately-empty stream with no error explaining why.
    if (session->terminated()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "session '" + first.session_id() + "' is already terminated");
    }

    // S5: this stream SUBSCRIBES. N streams may attach to one session; the
    // broadcaster owns the fan-out (one drop-oldest queue per stream, backlog
    // seeding for the first subscriber, a resync notice for a later one), so
    // there is no single-consumer claim to race for and no ALREADY_EXISTS to
    // refuse with. The old refusal existed because one queue had one consumer.
    // NotifyAttached is this layer's obligation in exchange for owning
    // Subscribe/Unsubscribe directly: it keeps ever_attached()/
    // has_subscribers() (the reaper's initial-attach guard and detached-idle
    // rule) accurate now that Session itself cannot see subscriber_count().
    //
    // Declared here (not `const auto` inline) and populated inside the try
    // below: Subscribe()/EmitSessionMetadata() are not the trivial calls they
    // look like -- EmitSessionMetadata's preset_snapshot() copies the whole
    // resolved preset (system prompt, tool_config, permission maps) under the
    // session lock, and Subscribe() allocates a queue -- so both can throw
    // (OOM, a lock-order surprise). `attached` records whether NotifyAttached
    // actually ran, so finish()'s cleanup below never fires NotifyDetached
    // without a matching attach: that pairing is what keeps
    // has_subscribers()/the detached-idle reaper accurate, and an unguarded
    // NotifyDetached on a Subscribe-throws path would leave a session
    // permanently unreapable (has_subscribers() reads underflowed-nonzero)
    // instead of merely failing this one RPC.
    rpc::EventBroadcaster::Handle subscriber;
    bool attached = false;

    // The one and only Write() caller for this stream. It drains THIS stream's
    // subscriber queue, not a shared one, so concurrent streams never split
    // each other's events.
    std::thread writer;
    bool terminate_requested = false;
    auto finish = [&](bool terminate, grpc::Status status) -> grpc::Status {
        // Order is the whole point, and it DIFFERS by outcome:
        //  * TERMINATE: Terminate broadcasts SessionEndedEvent to every
        //    subscriber and closes every queue drain-first -- so a client
        //    that explicitly asked to terminate (and is still reading) SEES
        //    the terminal event before its stream ends. The writer drains
        //    and unwinds from Pop; the join below cannot hang.
        //  * DETACH (and every bare drop): only THIS stream's queue closes,
        //    which is what unwinds a writer parked in Pop.
        //
        // Why no TryCancel before the join despite the wedge risk: a writer
        // can sit in a flow-control-blocked Write to a client that stopped
        // reading, and sync gRPC's Write has no deadline -- only
        // context->TryCancel() unblocks it, and TryCancel REPLACES whatever
        // status this handler returns with CANCELLED on the wire. That would
        // turn every graceful bare-drop close (client half-closes, then
        // waits on Finish for OK) into an error, breaking the ordinary
        // client. The wedge therefore stays a known trade: it requires a
        // connected-but-deaf client, and the queue close above unwinds the
        // writer in every well-behaved case.
        if (terminate) sessions_.Terminate(session->info().session_id);
        if (attached) {
            Transport(*session).queue().Unsubscribe(subscriber);
            session->NotifyDetached();
        }
        if (writer.joinable()) writer.join();
        // The attached_ slot is released by client_slot's destructor, after
        // this lambda returns -- the decrement (and, at zero, the last-client
        // callback) still precede the RPC handler's return.
        return status;
    };
    try {
        subscriber = Transport(*session).queue().Subscribe();
        session->NotifyAttached();
        attached = true;

        // First on the stream, before anything the client's first event
        // produces: a freshly attached tab has everything its status bar
        // renders from frame one. Targeted at THIS subscriber -- the other
        // streams attached already have their metadata. This is the RPC
        // thread: PublishTo never blocks.
        EmitSessionMetadata(*session, subscriber);

        writer = std::thread([stream, session, subscriber] {
            while (auto event = Transport(*session).queue().Pop(subscriber)) {
                if (!stream->Write(*event)) break;  // client went away
            }
        });

        // has_end_session() must be checked on `first` too: session_id and
        // end_session are both fields on the same ClientEvent, so a client is
        // free to name the session and ask to end it in one message. Routing
        // that into Dispatch() would silently no-op it (no kEndSession case
        // there -- the decision belongs here, where the read loop can unwind
        // around it) and this stream would then sit reading for a session it
        // already asked to leave.
        if (first.has_end_session()) {
            // The mode is REQUIRED, not defaulted: MODE_UNSPECIFIED fell
            // through `!= DETACH` into TERMINATE, so a client that sent
            // end_session{} without a mode (a proto3 zero-value slip) stopped
            // the session and destroyed its in-flight turn. A bare stream
            // drop already detaches; the one destructive outcome must be
            // spelled.
            if (first.end_session().mode() == v1::EndSession::MODE_UNSPECIFIED) {
                return finish(
                    /*terminate=*/false,
                    grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "end_session.mode is required (TERMINATE or DETACH); refusing "
                                 "to guess the destructive default"));
            }
            terminate_requested = first.end_session().mode() != v1::EndSession::DETACH;
        } else {
            Dispatch(first, *session);

            v1::ClientEvent event;
            while (stream->Read(&event)) {
                if (event.has_end_session()) {
                    // Handled HERE, not in Dispatch: the decision ends THIS
                    // stream's read loop, and only the handler knows how to
                    // unwind around it. TERMINATE stops the session; DETACH is
                    // the graceful form of a drop.
                    if (event.end_session().mode() == v1::EndSession::MODE_UNSPECIFIED) {
                        return finish(
                            /*terminate=*/false,
                            grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "end_session.mode is required (TERMINATE or DETACH); "
                                         "refusing to guess the destructive default"));
                    }
                    terminate_requested =
                        event.end_session().mode() != v1::EndSession::DETACH;
                    break;
                }
                // The stream is bound to the session `first` named. A later
                // event naming a DIFFERENT session is a client routing bug;
                // the field is redundant by design ("self-describing
                // messages"), so it is tolerated -- but never silently, or a
                // misrouted event would be undiagnosable.
                if (!event.session_id().empty() && event.session_id() != first.session_id()) {
                    logger_.warn("session {}: stream received an event naming session '{}'; "
                                 "routing bug on the client",
                                 first.session_id(), event.session_id());
                }
                Dispatch(event, *session);
            }
        }
    } catch (const std::exception& e) {
        // Unwinding without restoring the attach count would leak it, and the
        // refcounted server would then never believe it has zero clients. The
        // Dispatch branches are non-throwing by contract; this guard exists so
        // that contract can be violated without bricking shutdown.
        logger_.warn("session {}: OpenSession failed: {}", first.session_id(), e.what());
        return finish(/*terminate=*/true,
                      grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
    } catch (...) {
        // The SAME containment for an exception of an unknown flavor: the
        // contract above was guarded only against std::exception, so anything
        // else would have unwound past the refcount restore and bricked
        // shutdown outright.
        logger_.warn("session {}: OpenSession failed (unknown exception)", first.session_id());
        return finish(/*terminate=*/true,
                      grpc::Status(grpc::StatusCode::INTERNAL, "unknown exception"));
    }

    // S5 LIFETIME: a bare stream drop DETACHES -- a crashed terminal must not
    // destroy in-flight work, which is the whole feature. The session stays
    // live with zero subscribers: its turn keeps going, output lands in the
    // SQLite transcript, and the idle reaper collects it only after it has
    // been fully quiescent for sessions.detached_idle_ttl_seconds. The
    // never-attached guard is untouched: a stream that never named a session
    // never reaches this path, and a session nobody ever attaches to still
    // dies on the guard. Exceptions still terminate (conservative).
    return finish(/*terminate=*/terminate_requested, grpc::Status::OK);
}

void SessionServiceImpl::ReleaseClientSlot() {
    // ONE decrement of attached_, used by EVERY exit of OpenSession, because
    // the exits used to differ in the half that matters. The catch path once
    // restored the count and stopped there -- so a client whose stream unwound
    // decremented to zero without ever firing the callback, and main() wires
    // that callback to RequestShutdown(). The whole no-daemon design
    // ("refcounted by attached clients, gone when the last one leaves") then
    // never fired: the process held the socket forever and the next
    // auto-spawning client found a zombie it could not replace. The catch
    // block existed precisely for a stream that unwinds, and its own comment
    // said it was there so the refcount cannot leak -- it just did not carry
    // the consequence of reaching zero. Firing the last-client callback is
    // part of the decrement, not something callers may do selectively.
    if (attached_.fetch_sub(1, std::memory_order_acq_rel) == 1 && on_last_client_detached_) {
        on_last_client_detached_();
    }
}

grpc::Status SessionServiceImpl::GetMessages(grpc::ServerContext* context,
                                             const v1::GetMessagesRequest* request,
                                             v1::GetMessagesResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (request->session_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "session_id is required");
    }
    // Existence is checked separately because an unknown session and a session
    // with no messages both page as empty, and the client needs to tell them
    // apart -- one is a bug, the other is a brand-new tab. Guarded like every
    // sibling: a store fault (locked database, I/O error) must not leave the
    // handler via an exception (UB).
    bool exists = false;
    try {
        exists = db_.SessionExists(request->session_id());
    } catch (const std::exception& e) {
        logger_.warn("GetMessages({}): existence check failed: {}", request->session_id(),
                     e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!exists) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no such session: " + request->session_id());
    }

    const int limit = ClampPageSize(request->limit(), ScrollbackPageSize());

    try {
        if (request->has_after_seq() && request->has_before_seq()) {
            // The two cursors page in opposite directions; honoring both has
            // no coherent meaning, so name the misuse instead of silently
            // picking one.
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "after_seq and before_seq are mutually exclusive");
        }
        // has_before_seq()/has_after_seq(), not value tests: the wire fields
        // are optional precisely so "page from the newest" and "page before
        // seq 0" stay distinguishable.
        if (request->has_after_seq()) {
            // Delta fetch: only the rows the client does not hold, ascending.
            const auto page =
                reader_.GetMessagesAfter(request->session_id(), request->after_seq(), limit);
            for (const auto& stored : page.messages) {
                *response->add_messages() = ToProto(stored);
            }
            for (const auto& record : page.compactions) {
                *response->add_compactions() = ToProto(record);
            }
            // Note the flipped meaning vs. the before_seq path: more NEWER
            // rows remain (the page was full), not older pages -- see the
            // proto comment.
            response->set_has_more(page.has_more);
            return grpc::Status::OK;
        }
        const std::optional<std::int64_t> cursor =
            request->has_before_seq() ? std::optional<std::int64_t>(request->before_seq())
                                      : std::nullopt;
        const auto page = reader_.GetMessages(request->session_id(), cursor, limit);
        for (const auto& stored : page.messages) {
            *response->add_messages() = ToProto(stored);
        }
        for (const auto& record : page.compactions) {
            *response->add_compactions() = ToProto(record);
        }
        response->set_has_more(page.has_more);
    } catch (const std::exception& e) {
        // The codec is strict on decode by design: an undecodable row means
        // corruption or a database written by a newer binary. Surfacing it as
        // INTERNAL keeps that visible instead of returning a short page that
        // looks like the end of the transcript.
        logger_.warn("GetMessages({}): {}", request->session_id(), e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::GetSession(grpc::ServerContext* context,
                                            const v1::GetSessionRequest* request,
                                            v1::SessionSummary* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;
    if (request->session_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "session_id is required");
    }

    // GetSession throws on a store failure (locked database, I/O error,
    // corruption) rather than answering "no such session" -- same mapping the
    // resume path uses for the same call.
    std::optional<session::SessionSummary> stored;
    try {
        stored = db_.GetSession(request->session_id());
    } catch (const std::exception& e) {
        logger_.warn("GetSession({}): {}", request->session_id(), e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!stored) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no such session: " + request->session_id());
    }
    *response = ToProto(*stored);
    // Liveness from the registry, not the row -- the caller's attach-vs-resume
    // decision reads THIS flag, so it must answer the same question
    // ListSessions' is_live does.
    response->set_is_live(sessions_.Find(stored->session_id) != nullptr);
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ListSessions(grpc::ServerContext* context,
                                              const v1::ListSessionsRequest* request,
                                              v1::ListSessionsResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    session::ListSessionsFilter filter;
    if (!request->include_all_dirs()) {
        if (request->project_root().empty()) {
            // Without this the query would match sessions whose project_root is
            // literally "" -- an empty result that reads like "no sessions here"
            // rather than like the caller forgetting a field.
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "project_root is required unless include_all_dirs is set");
        }
        filter.project_root = request->project_root();
    }
    filter.include_subagents = request->include_subagents();
    filter.limit = ClampPageSize(request->limit(), ScrollbackPageSize());
    // Both halves or neither: the cursor is a (updated_at, id) pair because the
    // sort key is, and half a cursor would silently page from the wrong place.
    if (request->has_before_updated_at() && request->has_before_id()) {
        filter.before_updated_at = request->before_updated_at();
        filter.before_id = request->before_id();
    } else if (request->has_before_updated_at() || request->has_before_id()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "before_updated_at and before_id must be set together");
    }

    try {
        for (const auto& summary : db_.ListSessions(filter)) {
            auto* proto_summary = response->add_sessions();
            *proto_summary = ToProto(summary);
            // S5.4: from the live registry, not the stored row -- true iff a
            // Session object for this id exists right now (attached or
            // detached-but-running). One Find() per row; ListSessions pages
            // are bounded (ClampPageSize above), so this is not an unbounded
            // registry-mutex-under-lock scan.
            proto_summary->set_is_live(sessions_.Find(summary.session_id) != nullptr);
        }
    } catch (const std::exception& e) {
        logger_.warn("ListSessions: {}", e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ListAgentPresets(grpc::ServerContext* context,
                                                  const v1::ListAgentPresetsRequest* request,
                                                  v1::ListAgentPresetsResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (presets_ == nullptr) return grpc::Status::OK;  // empty list; see set_presets
    for (const auto& summary : presets_->ListAvailable(request->project_root())) {
        auto* proto_summary = response->add_presets();
        proto_summary->set_name(summary.name);
        proto_summary->set_description(summary.description);
        proto_summary->set_color(summary.color);
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ListModels(grpc::ServerContext* context,
                                            const v1::ListModelsRequest* request,
                                            v1::ListModelsResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (presets_ == nullptr) return grpc::Status::OK;  // empty list; see set_presets
    const auto& providers = presets_->providers();

    // refresh_discovery is the /providers refresh role, relocated into the
    // picker: an explicit re-fetch of every discover-entry's catalog before
    // listing. It is caller-requested network I/O with a bounded cost (each
    // provider's own gate, all providers in parallel), which is why the listing
    // RPC ITSELF never triggers a fetch -- a cold cache simply lists what is
    // already known (static models), and the picker tells the user about the
    // rest.
    if (request->refresh_discovery()) providers.RefreshDiscovery();

    // ModelRefs is static ∪ cached-discovered, mutex-guarded, sorted ascending.
    // Resolve per ref for the context window: a pure config lookup, no
    // provider I/O (the model object already exists; a discover-entry ref not
    // in the cache cannot appear here because ModelRefs lists only what is
    // materialized).
    for (const auto& ref_text : providers.ModelRefs()) {
        auto* proto_model = response->add_models();
        proto_model->set_ref(ref_text);
        if (const auto parsed = config::SplitModelRef(ref_text)) {
            if (const auto model = providers.Resolve(*parsed)) {
                if (model->context_window_tokens) {
                    proto_model->set_context_window_tokens(*model->context_window_tokens);
                }
            }
        }
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ListMcpServers(grpc::ServerContext* context,
                                                const v1::ListMcpServersRequest*,
                                                v1::ListMcpServersResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (mcp_manager_ == nullptr) return grpc::Status::OK;  // no mcps.json configured
    for (const auto& name : mcp_manager_->ServerNames()) {
        const auto status = mcp_manager_->StatusOf(name);
        auto* summary = response->add_servers();
        summary->set_name(name);
        summary->set_transport(status.transport == config::McpTransport::Http ? "http" : "stdio");
        summary->set_configured(status.configured);
        summary->set_enabled(status.enabled);
        summary->set_alive(status.alive);
        summary->set_error(status.error);
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::SetMcpServerEnabled(grpc::ServerContext* context,
                                                     const v1::SetMcpServerEnabledRequest* request,
                                                     v1::SetMcpServerEnabledResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (mcp_manager_ == nullptr) {
        response->set_ok(false);
        response->set_error("MCP is not available");
        return grpc::Status::OK;
    }
    std::string error;
    const bool ok = mcp_manager_->SetEnabled(request->name(), request->enabled(), error);
    response->set_ok(ok);
    response->set_error(error);
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::SuggestPaths(grpc::ServerContext* context,
                                              const v1::SuggestPathsRequest* request,
                                              v1::SuggestPathsResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    // Two scopes, mutually exclusive by contract (see the proto comment):
    // a session id, whose project root is looked up (live row first, database
    // fallback), or a bare project root for a SESSIONLESS tab -- a tab that
    // has not sent its first message yet owns no session row, and the
    // sessionless-tab model must not silence '@' autocomplete until one
    // exists. Setting both (or neither) is INVALID_ARGUMENT, not a silent
    // guess; a root that does not exist is NOT_FOUND, the same honest-answer
    // rule as the unknown-session case below -- an empty page would read as
    // "no matches here".
    std::filesystem::path project_root;
    if (!request->project_root().empty()) {
        if (!request->session_id().empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "session_id and project_root are mutually exclusive");
        }
        const std::filesystem::path root(request->project_root());
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "unknown project root: " + request->project_root());
        }
        project_root = root;
    } else if (!request->session_id().empty()) {
        // The project root of the session named in the request. A LIVE session wins;
        // a terminated one still answers from its database row, matching GetMessages,
        // which reads the same way whether or not a tab happens to be open. An id
        // that is neither is NOT_FOUND, not an empty result that reads like "no
        // matches here".
        if (const auto session = sessions_.Find(request->session_id())) {
            project_root = session->info().project_root;
        } else {
            // Guarded: GetSession can throw on a store failure (locked database,
            // I/O error), and an exception leaving an RPC handler is UB -- the
            // same rule ResumeSession's own comment states.
            std::optional<session::SessionSummary> row;
            try {
                row = db_.GetSession(request->session_id());
            } catch (const std::exception& e) {
                return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
            }
            if (!row) {
                return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                    "unknown session: " + request->session_id());
            }
            project_root = row->project_root;
        }
    } else {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "one of session_id or project_root is required");
    }

    const agent::MentionResolver& resolver =
        mention_resolvers_ != nullptr ? mention_resolvers_->Active() : DefaultMentionResolver();
    // Client-supplied page size, clamped like every other paged RPC: an
    // unclamped limit makes one keystroke enumerate a directory at arbitrary
    // depth of the response.
    const int limit = ClampPageSize(request->limit(), /*fallback=*/50);
    for (auto& path : resolver.Suggest(request->prefix(), project_root, limit)) {
        response->add_paths(std::move(path));
    }
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::PluginCall(grpc::ServerContext* context,
                                            const v1::PluginCallRequest* request,
                                            v1::PluginCallResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (services_ == nullptr) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "the plugin channel is not offered");
    }

    // Two dispatch conventions share this one generic RPC, and they cannot
    // collide: the E5 client-channel convention (PluginEndpoint) is Provided
    // under PluginEndpointServiceName(name) -- "<plugin_name>.endpoint" --
    // while the RPC-handler convention (PluginRpcHandler, e.g. checklist) is
    // Provided under PluginRpcServiceName(name) -- "<plugin_name>.rpc". Both
    // suffixed, deliberately: ServiceRegistry is one flat void*-typed
    // namespace shared with plain §2.2 services, and a bare plugin_name key
    // let ANY interface a plugin registered under its own name get
    // static_cast to PluginEndpoint here -- a real vtable call through the
    // wrong type, not a theoretical mismatch.
    if (void* service = services_->Find(plugins::PluginEndpointServiceName(request->plugin_name()));
        service != nullptr) {
        auto* endpoint = static_cast<plugins::PluginEndpoint*>(service);
        try {
            response->set_json_payload(endpoint->Call(request->method(), request->json_payload()));
            return grpc::Status::OK;
        } catch (const std::exception& e) {
            logger_.warn("plugin '{}' Call('{}') threw: {}", request->plugin_name(),
                        request->method(), e.what());
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "plugin endpoint threw: " + std::string(e.what()));
        } catch (...) {
            logger_.warn("plugin '{}' Call('{}') threw a non-standard exception",
                        request->plugin_name(), request->method());
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "plugin endpoint threw a non-standard exception");
        }
    }

    // Nothing registered under either convention: NOT_FOUND regardless of what
    // else is wrong with the request. Checked here, before delegating, because
    // DispatchPluginCall validates plugin_name/method emptiness BEFORE lookup
    // (its own documented contract for direct callers) -- which would answer
    // INVALID_ARGUMENT for a method-less call to a plugin that does not exist
    // under either name, when NOT_FOUND is the more useful (and tested) answer.
    if (services_->Find(plugins::PluginRpcServiceName(request->plugin_name())) == nullptr) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no plugin provides a service under '" + request->plugin_name() + "'");
    }

    // The dispatch itself (validation, resolution, handler invocation,
    // exception containment) is plugins::DispatchPluginCall, grpc-free so it is
    // unit-testable without a channel. Only the outcome -> status mapping
    // lives here.
    const auto result = plugins::DispatchPluginCall(services_, request->plugin_name(),
                                                    request->method(), request->json_payload());
    switch (result.outcome) {
        case plugins::RpcOutcome::kOk:
            response->set_json_payload(result.payload);
            return grpc::Status::OK;
        case plugins::RpcOutcome::kNotFound:
            return grpc::Status(grpc::StatusCode::NOT_FOUND, result.error);
        case plugins::RpcOutcome::kInvalid:
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, result.error);
        case plugins::RpcOutcome::kInternal:
            return grpc::Status(grpc::StatusCode::INTERNAL, result.error);
    }
    return grpc::Status(grpc::StatusCode::INTERNAL, "unreachable outcome");
}

grpc::Status SessionServiceImpl::ListCommands(grpc::ServerContext* context,
                                              const v1::ListCommandsRequest* request,
                                              v1::ListCommandsResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    // No registry wired: an empty list is the honest answer ("nothing to
    // autocomplete"), not an error -- the same session still answers an
    // invocation with "the /-command subsystem is not available".
    if (commands_ == nullptr) return grpc::Status::OK;

    const auto append = [response](const commands::CommandRegistry::Entry& entry) {
        const commands::Command* command = entry.command;
        // The SURFACE name -- the alias applied, the key Find() resolves --
        // not the command's own Name(): an aliased command advertised under
        // its original name would be undispatchable from the client.
        v1::CommandInfo* info = response->add_commands();
        info->set_name(entry.surface);
        info->set_summary(std::string(command->Summary()));
        info->set_usage(std::string(command->Usage()));
        info->set_read_only(command->ReadOnly());
        info->set_requires_session(command->RequiresSession());

        // The taxonomy IS the type (the same rule the busy-check runs on), so
        // the kind is derived from it rather than carried as a flag a command
        // could forget to keep in step with its own base class.
        if (dynamic_cast<const commands::PromptShorthandCommand*>(command) != nullptr) {
            info->set_kind(v1::COMMAND_KIND_PROMPT_SHORTHAND);
        } else if (dynamic_cast<const commands::LLMCommand*>(command) != nullptr) {
            info->set_kind(v1::COMMAND_KIND_LLM);
        } else {
            info->set_kind(v1::COMMAND_KIND_HARNESS);
        }

        switch (command->Display()) {
            case commands::CommandDisplay::kView:
                info->set_display(v1::COMMAND_DISPLAY_VIEW);
                break;
            case commands::CommandDisplay::kSilent:
                info->set_display(v1::COMMAND_DISPLAY_SILENT);
                break;
            case commands::CommandDisplay::kStatus:
                info->set_display(v1::COMMAND_DISPLAY_STATUS);
                break;
        }
    };

    // The effective catalog -- the same shadowing rule dispatch applies
    // (commands::EffectiveCatalog). request->project_root() empty (no
    // project_root given, or the requester has none yet) means global only:
    // For("") would be a permanently-empty registry anyway. The project slice
    // is resolved fresh per call, since a client may ask about a different
    // project_root on a later call within the same connection.
    // A session id names its own root (the client need not track roots per
    // tab); an unknown id falls back to the explicit project_root.
    std::filesystem::path project_root = request->project_root();
    if (!request->session_id().empty()) {
        if (const auto session = sessions_.Find(request->session_id())) {
            project_root = session->info().project_root;
        }
    }
    const std::shared_ptr<const commands::CommandRegistry> project_registry =
        project_commands_ != nullptr && !project_root.empty()
            ? project_commands_->For(project_root)
            : nullptr;
    for (const auto& entry :
         commands::EffectiveCatalog(*commands_, project_registry.get())) {
        append(entry);
    }
    return grpc::Status::OK;
}

namespace {

// Never consulted. DispatchSessionless refuses RequiresConfirmation commands
// before Execute, so the broker a CommandContext structurally requires has no
// sessionless caller; it exists to keep the context's shape uniform rather
// than to fork a second context type for one missing member.
class NullConfirmationBroker final : public commands::ConfirmationBroker {
    commands::ConfirmationResponse RequestConfirmation(
        const commands::ConfirmationRequest&) override {
        return {};
    }
};

}  // namespace

grpc::Status SessionServiceImpl::RunSessionlessCommand(
    grpc::ServerContext* context, const v1::RunSessionlessCommandRequest* request,
    v1::RunSessionlessCommandResponse* response) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    // Answered, never a gRPC error: the client maps the result onto its notice
    // line exactly like an on-stream command outcome, so refusals travel as
    // COMMAND_STATUS_ERROR payloads.
    auto* result = response->mutable_result();
    if (commands_ == nullptr || presets_ == nullptr) {
        result->set_status(v1::COMMAND_STATUS_ERROR);
        result->set_message("the /-command subsystem is not available");
        return grpc::Status::OK;
    }

    // Inline on the handler thread, deliberately. A sessionless command is
    // ReadOnly by the dispatcher's guard -- registry scans and listing
    // requests, the same bounded class of work CreateSession does here -- and
    // unlike the OpenSession reader thread there is no stream starving behind
    // this call, so the command pool earns nothing by being involved.
    static NullConfirmationBroker null_confirmations;
    commands::CommandContext command_context(/*session=*/nullptr, *presets_, null_confirmations,
                                             logger_, mcp_manager_);
    const commands::CommandResult outcome =
        commands::DispatchSessionless(*commands_, request->name(), FromProto(request->args()),
                                      command_context);
    FillProto(*result, outcome);
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::DeleteSession(grpc::ServerContext* context,
                                               const v1::DeleteSessionRequest* request,
                                               v1::DeleteSessionResponse*) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (request->session_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "session_id is required");
    }
    // A live session (attached OR detached -- S5) is a stream's subject:
    // deleting the row under a Session that appends to it would turn every
    // later write into a foreign-key error. Closing a tab only detaches now
    // (S5.2); the client sends EndSession{TERMINATE} (or waits for the idle
    // reaper to collect a quiescent detached session) before deleting.
    if (sessions_.Find(request->session_id()) != nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "session '" + request->session_id() +
                                "' is open; close it before deleting");
    }
    // Guarded: a store failure (locked database, I/O error) must not leave
    // the handler via an exception (UB), the same rule every other call
    // site in this file follows.
    bool deleted = false;
    try {
        deleted = db_.DeleteSession(request->session_id());
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!deleted) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no such session: " + request->session_id());
    }
    return grpc::Status::OK;
}

void SessionServiceImpl::EmitSessionMetadata(
    agent::Session& session, const rpc::EventBroadcaster::Handle& subscriber) {
    // Snapshots, never the live refs: this runs on the RPC thread while the
    // worker mutates preset_ and SetName/ApplyPreset mutate info_ fields
    // (name, agent_preset) under the session mutex -- an unlocked info() read
    // here raced a concurrent std::string write (torn read, UB).
    const auto info = session.info_snapshot();
    const auto preset = session.preset_snapshot();
    v1::ServerEvent event;
    event.set_session_id(info.session_id);
    auto* metadata = event.mutable_session_metadata();
    // Redundant with the envelope on purpose, like the message's own field
    // comment says: events are self-describing on their own.
    metadata->set_session_id(info.session_id);
    metadata->set_name(info.name);
    metadata->set_agent_preset(info.agent_preset);
    metadata->set_model(preset.model_ref.str());
    metadata->set_permission_mode(config::ToString(session.run_mode()));
    metadata->set_context_window_tokens(preset.context_window_tokens.value_or(0));
    // Targeted at the new subscriber only -- the streams already attached have
    // their metadata. Never blocks; a refusal means the stream went away mid-
    // attach, which the handler unwind below handles.
    if (!Transport(session).queue().PublishTo(subscriber, std::move(event))) {
        logger_.warn("session {}: stream closed during attach; SessionMetadataEvent dropped",
                     info.session_id);
    }
}

grpc::Status SessionServiceImpl::PluginSubscribe(grpc::ServerContext* context,
                                                 const v1::PluginCallRequest* request,
                                                 grpc::ServerWriter<v1::PluginCallResponse>* writer) {
    if (const auto auth = EnsureAuthenticated(auth_providers_, context); !auth.ok()) return auth;

    if (services_ == nullptr) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "the plugin channel is not offered");
    }
    void* service = services_->Find(plugins::PluginEndpointServiceName(request->plugin_name()));
    if (service == nullptr) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no plugin provides a service under '" + request->plugin_name() + "'");
    }
    auto* endpoint = static_cast<plugins::PluginEndpoint*>(service);

    // The sink serializes pushes (gRPC permits one concurrent Write) and
    // translates "client went away" into Push() == false, which is the
    // endpoint's signal to stop and drop its reference. Shared ownership: the
    // endpoint may hold the sink past this call's lifetime, and every Push
    // after close just fails -- no dangling writer.
    class WriterSink final : public plugins::PluginSubscriptionSink {
    public:
        explicit WriterSink(grpc::ServerWriter<v1::PluginCallResponse>* writer)
            : writer_(writer) {}

        bool Push(std::string json_payload) override {
            std::lock_guard<std::mutex> lock(mtx_);
            if (closed_.load(std::memory_order_acquire)) return false;
            v1::PluginCallResponse response;
            response.set_json_payload(std::move(json_payload));
            if (!writer_->Write(response)) {
                closed_.store(true, std::memory_order_release);
                cv_.notify_all();
            }
            return !closed_.load(std::memory_order_acquire);
        }

        // Takes the SAME mutex Push() holds across writer_->Write(): without
        // it, the handler could return (destroying the ServerWriter) while a
        // plugin thread was still inside Write() on it -- a use-after-free.
        // Locking here waits out any in-flight Push before this call returns.
        void Close() {
            std::lock_guard<std::mutex> lock(mtx_);
            closed_.store(true, std::memory_order_release);
            cv_.notify_all();
        }
        bool closed() const { return closed_.load(std::memory_order_acquire); }

        // Blocks the handler thread until Push()/Close() sets closed_, or
        // `timeout` elapses (the caller's IsCancelled() backstop, since that
        // is the only client-disconnect signal the sync API offers). Replaces
        // a 20ms busy-poll that pinned one gRPC thread per live subscription
        // at 50 wakeups/s for the subscription's whole life (a checklist or
        // cost-tracker view can hold one open for a session's entire
        // duration).
        void WaitUntilClosed(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, timeout, [this] { return closed_.load(std::memory_order_acquire); });
        }

    private:
        grpc::ServerWriter<v1::PluginCallResponse>* writer_;
        std::mutex mtx_;
        std::condition_variable cv_;
        std::atomic<bool> closed_{false};
    };

    auto sink = std::make_shared<WriterSink>(writer);
    // Plugin code is not trusted to return normally (the same rule PluginCall's
    // dispatch states), and an exception leaving an RPC handler is UB.
    try {
        if (!endpoint->Subscribe(request->method(), request->json_payload(), sink)) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "plugin '" + request->plugin_name() +
                                    "' refused subscription to '" + request->method() + "'");
        }
    } catch (const std::exception& e) {
        logger_.warn("plugin '{}' Subscribe('{}') threw: {}", request->plugin_name(),
                     request->method(), e.what());
        // Subscribe may have registered the sink before throwing -- the endpoint
        // pushes from threads it owns -- so close it on the unwind; every
        // straggler Push then fails harmlessly instead of outliving the stream.
        sink->Close();
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "plugin endpoint threw: " + std::string(e.what()));
    } catch (...) {
        logger_.warn("plugin '{}' Subscribe('{}') threw a non-standard exception",
                     request->plugin_name(), request->method());
        sink->Close();
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "plugin endpoint threw a non-standard exception");
    }

    // The call stays open while the subscriber lives. The endpoint pushes from
    // its own threads; this thread wakes immediately on a Push failure or an
    // explicit Close (the condition variable), and otherwise wakes only every
    // 500ms to check IsCancelled() -- the one client-disconnect signal the
    // sync API offers that nothing else can push a wakeup for.
    //
    // A live plugin subscription IS an attached client: attached_clients() is
    // what the idle-exit reaper consults, and a checklist or cost-tracker view
    // holding a stream open for a session's whole duration used to be
    // invisible to it -- the process could conclude it was idle and shut down
    // under a live stream. Counted only while the stream is actually live
    // (every refusal path above already returned), and released on every exit
    // of the scope, exceptions included.
    const AttachedClientSlot subscription_slot(this);
    while (!sink->closed() && !context->IsCancelled()) {
        sink->WaitUntilClosed(std::chrono::milliseconds(500));
    }
    sink->Close();
    return grpc::Status::OK;
}

}  // namespace minimal::rpc
