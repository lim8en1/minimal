#include "minimal/rpc/session_service.hpp"

#include <algorithm>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

#include "minimal/rpc/proto_convert.hpp"

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

grpc::Status NotYet(const char* what) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                        std::string(what) + " is not implemented yet");
}

}  // namespace

SessionServiceImpl::SessionServiceImpl(agent::SessionRegistry& sessions,
                                       session::SessionDatabase& db,
                                       config::SettingsConfig settings,
                                       config::PermissionsConfig permissions,
                                       common::Logger& logger,
                                       const tools::PermissionAdvisorRegistry* permission_advisors)
    : sessions_(sessions),
      db_(db),
      reader_(db),
      settings_(std::move(settings)),
      permission_advisors_(permission_advisors),
      permissions_(std::move(permissions)),
      logger_(logger) {}

namespace {

// The advisor composition for one session: the channels block comes first
// (the wire checker is built against it), then the advised checker owns that
// wire checker, and the shared cancel holder travels in params so the Session
// can install its own predicate. No advisor configured, or the named plugin
// not loaded (a runtime condition the sanity check warns about): params carry
// no checker and Session builds the wire default -- the human, as always.
void ApplyAdvisor(const config::PermissionsConfig& permissions,
                  const tools::PermissionAdvisorRegistry* advisors, agent::SessionParams& params) {
    if (!permissions.advisor || advisors == nullptr) return;
    auto advisor = advisors->Find(*permissions.advisor);
    if (advisor == nullptr) return;
    params.channels = agent::MakeSessionChannels();
    auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
    params.permission_checker = std::make_unique<tools::AdvisedPermissionChecker>(
        std::move(advisor),
        std::make_unique<WirePermissionChecker>(params.session_id, params.channels->queue,
                                                params.channels->pending),
        cancel_source, std::chrono::milliseconds(permissions.advisor_timeout_ms));
    params.advisor_cancel_source = std::move(cancel_source);
}

}  // namespace

void SessionServiceImpl::set_on_last_client_detached(std::function<void()> callback) {
    on_last_client_detached_ = std::move(callback);
}

std::size_t SessionServiceImpl::attached_clients() const {
    return attached_.load(std::memory_order_acquire);
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
        if (::access(params.project_root.c_str(), W_OK) != 0) {
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
                                params.agent_preset);
    } catch (const std::exception& e) {
        logger_.warn("CreateSession: could not record session: {}", e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    params.session_id = row.session_id;

    // After the id is minted: the wire checker inside the composition captures
    // the session_id, so the advisor cannot be applied earlier.
    ApplyAdvisor(permissions_, permission_advisors_, params);

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
        db_.DeleteSession(row.session_id);
        logger_.warn("CreateSession failed: {}", e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }

    *response->mutable_session() = ToProto(row);
    return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ApplyModelOverride(const v1::CreateSessionRequest& request,
                                                    agent::SessionParams& params) const {
    // Shape-checked here so a malformed reference is INVALID_ARGUMENT rather
    // than FAILED_PRECONDITION from deep inside session construction -- the
    // client sent something wrong, which is a different thing from the server
    // not being able to honour something well-formed.
    if (request.model().empty()) return grpc::Status::OK;
    params.model_override = config::ParseModelRef(request.model());
    if (!params.model_override) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "model '" + request.model() +
                                "' must be <provider>/<model>, e.g. local/qwen4");
    }
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

    const auto stored = db_.GetSession(request->session_id());
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
    ApplyAdvisor(permissions_, permission_advisors_, params);
    // APPLIES ON RESUME TOO, like run_mode and for the same reason: a model the
    // user names is a property of this invocation, not of the stored session --
    // and the override is in-memory only, so a resumed session has none of its
    // own to preserve. "Continue this conversation, but on the big model" is the
    // obvious thing to want. Silently dropping it would be exactly the failure
    // the field checks above refuse to make for name/preset/project_root.
    if (const auto status = ApplyModelOverride(*request, params); !status.ok()) return status;

    try {
        // Loads the stored transcript, repairs a torn tool-call tail if there
        // is one, and starts the session.
        (void)sessions_.Create(std::move(params));
    } catch (const std::exception& e) {
        // NO ROLLBACK here, unlike fresh creation: the row is the user's
        // existing session, not one this call just minted. A failure (the
        // stored preset no longer resolves, its provider kind no longer
        // registered) is reported with its reason and the transcript survives.
        logger_.warn("resume of session {} failed: {}", stored->session_id, e.what());
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
    }

    // Re-read rather than reuse `stored`: resume may have appended a repair
    // message, and the response should carry the current message_count.
    const auto fresh = db_.GetSession(stored->session_id);
    *response->mutable_session() = ToProto(*fresh);
    return grpc::Status::OK;
}

void SessionServiceImpl::Dispatch(const v1::ClientEvent& event, agent::Session& session) {
    switch (event.payload_case()) {
        case v1::ClientEvent::kChatTurn:
            // Queues and returns. Running the turn here would block the reader,
            // which is the deadlock this whole topology exists to avoid.
            session.SubmitTurn(event.chat_turn().text());
            return;

        case v1::ClientEvent::kPermissionResponse:
            // Carried across as the typed variant, still unparsed: parsing
            // belongs on the worker, which can afford to fail, not on the
            // reader, which must not stall. The payload case IS the type --
            // the variant makes a wrong-kind answer structurally unreadable
            // at the waiter rather than merely failing to parse there.
            session.pending().Resolve(event.permission_response().request_id(),
                                      PermissionAnswer{event.permission_response().SerializeAsString()});
            return;

        case v1::ClientEvent::kConfirmationResponse:
            session.pending().Resolve(event.confirmation_response().request_id(),
                                      ConfirmationAnswer{event.confirmation_response().chosen_option()});
            return;

        case v1::ClientEvent::kCancel:
            session.Cancel();
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

        case v1::ClientEvent::PAYLOAD_NOT_SET:
            logger_.warn("session {}: client event with no payload", session.info().session_id);
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
    const commands::Command* command =
        commands_ != nullptr ? commands_->Find(queued.name) : nullptr;
    if (command != nullptr && command->ReadOnly()) {
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
        session.RejectCommand(queued, "/" + queued.name +
                                          " cannot run while a turn is in flight; cancel it first");
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
        session.RejectCommand(queued, "unknown command: /" + queued.name + " (try /help)");
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

    if (!session->TryAttach()) {
        // One stream per session. Attaching anyway would silently split the
        // event stream between two writers draining one queue, and the first to
        // close would terminate the session under the other -- both of which
        // look like flaky output rather than a protocol error.
        return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                            "session '" + first.session_id() + "' already has a client attached");
    }
    // A session the reaper or another stream is already tearing down must not be
    // attached to at all: its queue is closing, and the client would see an
    // immediately-empty stream with no error explaining why.
    if (session->terminated()) {
        session->Detach();
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "session '" + first.session_id() + "' is already terminated");
    }
    attached_.fetch_add(1, std::memory_order_acq_rel);

    // The one and only Write() caller for this stream. It drains the SESSION's
    // queue, not a stream-owned one, so anything produced before this moment is
    // still here and goes out first.
    std::thread writer;
    try {
        writer = std::thread([stream, session] {
            while (auto event = session->queue().Pop()) {
                if (!stream->Write(*event)) break;  // client went away
            }
        });

        Dispatch(first, *session);

        v1::ClientEvent event;
        while (stream->Read(&event)) {
            Dispatch(event, *session);
        }
    } catch (const std::exception& e) {
        // Unwinding without restoring the attach count would leak it, and the
        // refcounted server would then never believe it has zero clients. The
        // Dispatch branches are non-throwing by contract; this guard exists so
        // that contract can be violated without bricking shutdown.
        logger_.warn("session {}: OpenSession failed: {}", first.session_id(), e.what());
        sessions_.Terminate(session->info().session_id);
        session->queue().Close();
        if (writer.joinable()) writer.join();
        ReleaseAttachment(*session);
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }

    // Stream close TERMINATES the session -- there is no detached state in v1,
    // so a dropped stream is unambiguous. Terminate() closes the queue (before
    // joining the worker, so a worker parked on a full queue is woken rather
    // than deadlocked), which is what lets the writer finish rather than
    // blocking forever on Pop().
    sessions_.Terminate(session->info().session_id);
    if (writer.joinable()) writer.join();
    ReleaseAttachment(*session);
    return grpc::Status::OK;
}

void SessionServiceImpl::ReleaseAttachment(agent::Session& session) {
    // ONE function, called by both exits, because the two used to differ in the
    // half that matters. The catch path restored the count and stopped there --
    // so a client whose stream unwound decremented to zero without ever firing
    // the callback, and main() wires that callback to RequestShutdown(). The
    // whole no-daemon design ("refcounted by attached clients, gone when the
    // last one leaves") then never fired: the process held the socket forever
    // and the next auto-spawning client found a zombie it could not replace.
    //
    // The catch block existed precisely for a stream that unwinds, and its own
    // comment says it is there so the refcount cannot leak -- it just did not
    // carry the consequence of reaching zero. Splitting that decision across two
    // call sites is what let them drift, so there is now only one.
    session.Detach();
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
    // apart -- one is a bug, the other is a brand-new tab.
    if (!db_.SessionExists(request->session_id())) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "no such session: " + request->session_id());
    }

    const int limit = ClampPageSize(request->limit(), ScrollbackPageSize());

    try {
        // has_before_seq(), not a value test: the wire field is optional
        // precisely so "page from the newest" and "page before seq 0" stay
        // distinguishable.
        const std::optional<std::int64_t> cursor =
            request->has_before_seq() ? std::optional<std::int64_t>(request->before_seq())
                                      : std::nullopt;
        const auto page = reader_.GetMessages(request->session_id(), cursor, limit);
        for (const auto& stored : page.messages) {
            *response->add_messages() = ToProto(stored);
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
            *response->add_sessions() = ToProto(summary);
        }
    } catch (const std::exception& e) {
        logger_.warn("ListSessions: {}", e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    return grpc::Status::OK;
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
