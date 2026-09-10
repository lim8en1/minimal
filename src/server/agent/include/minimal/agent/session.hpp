#pragma once

// One open session: the agent loop, its tools, its provider, its transcript, and
// the worker thread all of that runs on.
//
// THREE THREADS PER SESSION, and the split is the reason this class exists
// rather than the loop being driven from the RPC handler (ARCHITECTURE.md
// "Concurrency model"):
//
//   Reader  -- the gRPC handler thread. Never blocks on anything but Read().
//              Calls SubmitTurn / Cancel / resolves a pending permission or
//              confirmation request through the transport and returns.
//   Worker  -- owned here. Blocks freely: permission waits, provider calls,
//              bash subprocesses.
//   Writer  -- owned by the transport, drains its subscriber queue via
//              Pop(handle) and is the sole Write() caller for its stream.
//
// Running the loop on the reader thread would deadlock on the first gated tool
// call: check() would block the very thread that has to deliver the answer.
//
// TERMINATION REUSES THE CANCELLATION PATH -- atomic flag, then the bool stream
// sink aborts the provider request, then tools observe is_cancelled -- and adds
// no second mechanism. See "Session lifetime".

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include "minimal/agent/agent_loop.hpp"
#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/injection_queue.hpp"
#include "minimal/agent/session_event.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_dispatcher.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/tools/ask.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/session/history_store.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::mcp {
class McpClientManager;       // borrowed via SessionParams::mcp_manager
}  // namespace minimal::mcp

namespace minimal::agent {

class ProviderInstanceCache;  // borrowed via set_provider_instance_cache
class SignalRouter;           // borrowed via SessionParams::signal_router

// Session's domain-only cross-thread state -- the half of what this block used
// to bundle that has nothing to do with the wire. The transport half
// (EventBroadcaster, PendingRequests) split out to rpc::TransportChannels
// (see rpc/transport_channels.hpp) once Session stopped touching either
// directly; this block keeps existing, by shared_ptr, for exactly the same
// reason it always did -- background watcher threads capture it and may
// outlive both the worker and Session itself.
struct SessionChannels {
    // Live background tasks: STARTED increments, a terminal state decrements.
    // Lives HERE, on the shared_ptr block, because the task_event_sink that
    // maintains it is captured by background watchers that may fire after the
    // Session is gone.
    std::atomic<std::size_t> background_tasks_live{0};
    // Cross-thread staging for out-of-band messages (non-blocking subagent
    // completions, background task results), drained at turn boundaries. Lives
    // HERE -- on the shared_ptr block -- because that is exactly the object a
    // cross-thread handle must outlive in order to be safe: a tool's captured
    // schedule_injection holds the channels shared_ptr, so the queue (and the
    // callables' state) survive past the session and Push refuses after
    // Terminate closes it. See injection_queue.hpp.
    InjectionQueue injections;
};

inline std::shared_ptr<SessionChannels> MakeSessionChannels() {
    return std::make_shared<SessionChannels>();
}

struct SessionParams {
    std::string session_id;
    std::string parent_session_id;  // empty for a top-level session
    std::string name;
    std::string agent_preset;
    std::filesystem::path project_root;
    // The permission decision source for this session. Null means the wire
    // checker (the default -- prompts round-trip to the client). A producer
    // that configured an advisor composes AdvisedPermissionChecker(wire) and
    // injects it; a test injects any fake. ToolExecutor sees one checker
    // either way. Populated for every real session by
    // rpc::FinishSessionWiring, called from SessionRegistry::CreateResolved --
    // see that function for why Session's constructor cannot build the
    // default itself any more.
    std::unique_ptr<tools::PermissionChecker> permission_checker;
    // Set only when permission_checker is an AdvisedPermissionChecker composed
    // around this shared holder: Session installs its own cancel predicate into
    // it in the constructor body, before the worker starts. Null otherwise.
    std::shared_ptr<tools::AdvisorCancelSource> advisor_cancel_source;
    // Where every SessionEvent Session emits goes. Built by
    // rpc::ApplyPermissionAdvisor/FinishSessionWiring against a real
    // rpc::EventTranslator; a test injects any fake sink. Required -- Session's
    // constructor throws if this is unset, rather than silently dropping every
    // event, since nothing about a missing sink is otherwise observable until
    // a client notices events never arrive.
    SessionEventSink event_sink;
    // Always the wire default (rpc::WireConfirmationBroker) in production --
    // unlike permission_checker, there is no advisor for confirmations to
    // decorate. A test injects any fake. Required, same reasoning as
    // event_sink.
    std::unique_ptr<commands::ConfirmationBroker> confirmation_broker;
    // Always the wire default (rpc::WireAskBroker) in production. OPTIONAL,
    // unlike confirmation_broker: a session without it simply exposes no
    // interactive ask -- ToolContext::ask_user stays null and the tool-ask
    // plugin's tool degrades to an error result. Null keeps test fixtures
    // that care nothing about asks from having to build one.
    std::unique_ptr<tools::AskBroker> ask_broker;
    // Opaque handle to whatever rpc::EventTranslator backs event_sink, kept
    // alive here so callers that need the underlying rpc::EventBroadcaster/
    // PendingRequests (SessionServiceImpl's Subscribe/Unsubscribe/Resolve) can
    // reach it via Session::transport() without agent/ ever naming the
    // concrete rpc:: type. Session itself never dereferences this.
    std::shared_ptr<void> transport_handle;
    // Terminate()/Cancel()'s hooks into the transport's PendingRequests, which
    // Session can no longer name directly. See PendingRequests::Shutdown/
    // CancelAll for what each does and why they are two different calls.
    std::function<void()> shutdown_pending_requests;
    std::function<void()> cancel_pending_requests;
    // Terminate()'s hook to close every subscriber queue on the transport
    // (EventBroadcaster::CloseAll), which Session can no longer name
    // directly. Called AFTER the terminal SessionEnded event is published, so
    // every attached writer drains it before unwinding.
    std::function<void()> close_transport;
    // Set when the client asked for a specific mode (the headless client asks
    // for AllowOnly); unset means the preset's configured mode stands. For a
    // subagent the spawner has ALREADY clamped it against the parent's via
    // clamp_run_mode -- a child is never more permissive than its parent.
    std::optional<tools::PermissionRunMode> run_mode_override;
    // Set when the user named a model explicitly (--model, later /model). Wins
    // over whatever the preset resolves to, and is re-applied after a preset
    // switch -- the preset is where a default comes from, not a decision.
    std::optional<config::ModelRef> model_override;
    // Persistence hook for model_override, wired by the registry (which owns
    // the database) with the session_id bound. Fired by set_model_override so
    // a runtime /model switch survives a server restart; null (test sessions,
    // spawners that persist the override themselves) means memory-only.
    std::function<void(const std::optional<config::ModelRef>&)> model_override_persist;
    // The rendered project-context section (AGENTS.md/CLAUDE.md), loaded by the
    // registry from project_root -- a SESSION property, not a preset one, so a
    // mid-session /agent switch cannot drop the project's instructions. The
    // loop appends it after the preset's system prompt on every request. Empty
    // when nothing was found (the normal state) or the feature is disabled.
    std::string project_context;
    // Pre-rendered signal-channel context (each subscribed channel's
    // instructions), composed by the session service from the router and
    // APPENDED to project_context by the registry's create funnel. Empty for
    // sessions with no subscriptions -- which is every subagent, since
    // subagents cannot subscribe. Travels alongside signal_router and
    // signal_selectors below as one group: all three are populated together
    // by ApplySignals and consumed together by CreateResolved.
    std::string signal_context;
    // Borrowed, like mcp_manager. Set by ApplySignals only when the request
    // declared selectors and a router is configured; null otherwise --
    // including every subagent, since subagents cannot subscribe.
    // CreateResolved uses this to register the session's subscriptions
    // atomically with making it findable (inside the same registry-mutex
    // critical section as the sessions_ insertion), closing the window
    // where an internal signal addressed to this session_id would find no
    // subscriber even though the CreateSessionRequest that created it had
    // already declared one.
    SignalRouter* signal_router = nullptr;
    // The raw selector strings from CreateSessionRequest.signals, already
    // validated by ApplySignals/CheckSelector. Consumed by CreateResolved
    // to call SignalRouter::Subscribe once per selector; empty whenever
    // signal_router is null.
    std::vector<std::string> signal_selectors;
    // True when this Session reopens a STORED session: the registry loads the
    // stored transcript as the loop's seeded history (without re-appending it)
    // before construction. The session row must already exist either way --
    // the database mints ids -- but a fresh session's transcript is empty.
    bool resume = false;

    // When an out-of-band message (a non-blocking subagent completion, a
    // background task's result) lands on an IDLE session, run a turn for it
    // immediately instead of waiting for the next user turn to flush it --
    // the injected messages ARE that turn's user content. Off by default: a
    // wake is a real provider call the user did not ask for, so it is a
    // settings.json decision (`background_wake`), never a default. Subagents
    // never wake (their life is one seeded turn, and they take the default).
    bool wake_on_injection = false;

    // The read side of the session's durable transcript, injected by the
    // registry (which owns the database). Session owns it and builds the
    // HistoryWindow that reaches tools as ToolContext::history
    // (docs/EXTENSIBILITY.md §2.1, track M). Null when a session's store is
    // not SQLite-backed -- plain test Sessions on fake stores -- which makes
    // ctx.history null: the natural "unavailable" case, not an error path.
    std::shared_ptr<const session::SessionReader> history_reader;

    // --- Subagent inheritance (empty/unset for a top-level session) ----------
    // Grants the parent already held at spawn time. Re-prompting for every
    // binary the parent's operator approved would make subagents unusable; a
    // copy is the decision the development plan records.
    std::vector<std::string> seed_grants;
    // The parent's policies, handed to the child's ToolExecutor as ceilings it
    // folds in via most_restrictive() -- the per-category "child is never more
    // permissive than its parent" rule.
    std::optional<tools::PermissionPolicy> policy_ceiling;
    std::optional<tools::McpPermissionPolicy> mcp_policy_ceiling;
    // Subagents only: fired ONCE on the worker when the session's single seeded
    // turn ends -- every exit path, success or failure, since a blocker waiting
    // on the result must unblock on failure too. The spawner uses it to capture
    // the final text, release a blocking parent, and queue a non-blocking
    // parent's completion injection.
    std::function<void(const TurnResult&)> on_turn_finished;

    // Borrowed, like the registry: the manager outlives every session. Null
    // (no mcps.json entries, or tests without MCP) simply means no live MCP
    // servers and no liveness checks.
    mcp::McpClientManager* mcp_manager = nullptr;

    // S5.3: sessions.unattended_prompt_timeout_seconds, set by the registry
    // funnel (SessionRegistry::set_prompt_timeout, mirroring
    // set_project_context). Zero -- the default -- means wait forever, the
    // behaviour every prompt had before detach existed. Applied to the
    // default WirePermissionChecker/WireConfirmationBroker this constructor
    // builds; a producer that supplies its own permission_checker owns its
    // own timeout policy.
    std::chrono::milliseconds unattended_prompt_timeout{0};

    // --- Advisor selection (Phase 5 step 3) -----------------------------------
    // A composition call site (SessionServiceImpl::ApplyAdvisor,
    // SubagentManager's advisor-inheritance path) sets these to record WHICH
    // advisor applies, if any -- pure selection, no wiring. Null means no
    // advisor. rpc::FinishSessionWiring, called from the one funnel every
    // session passes through, is what actually BUILDS the composed checker
    // from these, after unattended_prompt_timeout above has resolved.
    std::shared_ptr<tools::PermissionAdvisor> selected_advisor;
    std::chrono::milliseconds advisor_timeout{0};

    // --- Constructor-injected wiring (Phase 5 step 3) -------------------------
    // Formerly set on the live Session by SessionRegistry via set_presets/
    // set_commands/set_provider_factories/set_provider_instance_cache, AFTER
    // construction -- a window during which the worker thread already
    // existed but these were still null. All four borrowed, process-wide;
    // unset in tests that never exercise /agent or a provider rebuild.
    const AgentPresetRegistry* presets = nullptr;
    // Null for a subagent: commands are a user control channel a child has
    // none of (see WireSession's reasoning at the call site).
    const commands::CommandRegistry* commands = nullptr;
    const llm::ProviderFactoryRegistry* provider_factories = nullptr;
    ProviderInstanceCache* provider_instance_cache = nullptr;
};

// One item of work for the session worker. A variant rather than two queues,
// because the ORDER between a turn and a command matters and two queues have no
// order between them -- `/model x` typed before a turn must apply before it.
struct QueuedCommand {
    std::string request_id;  // echoed on CommandResult
    std::string name;
    commands::CommandArgs args;
};

// One traversal request for the session worker. A rewind cuts the ACTIVE
// transcript before target_seq (the target must be an active, text-bearing
// user message); a redo restores the tail the most recent rewind discarded.
// See ARCHITECTURE.md "Session traversal" and the RewindRequest proto comment
// for the mark/fork/reclaim model.
struct QueuedRewind {
    std::int64_t target_seq = -1;  // meaningful only when redo == false
    bool redo = false;
};

// One client-driven run-mode change (shift+tab's direct switch). Queued to
// the worker like a rewind because SetRunMode is worker-thread-only (the
// executor reads it lock-free during tool dispatch).
struct QueuedRunMode {
    tools::PermissionRunMode mode = tools::PermissionRunMode::Normal;
};

// A user turn carrying PER-TURN model/agent overrides. Applied for this turn
// only, on the worker at turn pop -- the sticky session preset, the session's
// model_override_, and every metadata event are untouched. The model rides as
// its RAW "<provider>/<model>" string and is split and resolved at pop: an
// unknown or malformed ref fails the turn with a named error, never a silent
// fall-back, and the reader never blocks on resolution.
struct QueuedTurn {
    std::string text;
    std::string model;         // raw "<provider>/<model>"; empty = keep the session's
    std::string agent_preset;  // empty = keep the session's own
};

using WorkItem =
    std::variant<std::string, QueuedTurn, QueuedCommand, QueuedRewind, QueuedRunMode>;  // string = a plain user turn

class Session : public commands::SessionControl {
public:
    // `provider` is owned because a session outlives any one turn and the
    // provider is stateless by contract; `registry` and `observers` are
    // process-wide and outlive every session.
    // `provider` and `store` are owned; `registry` and `observers` are
    // process-wide. The store is injected rather than constructed here so a
    // session does not have to know whether its transcript is durable -- and so
    // tests can drive one without a database file.
    // `seed` is the RESUME path: see ResumeSeed (agent_loop.hpp).
    // Default-constructed for a new session.
    Session(SessionParams params, AgentPreset preset, std::shared_ptr<llm::LlmProvider> provider,
            std::unique_ptr<session::HistoryStore> store, tools::ToolRegistry& registry,
            LoopConfig loop_config, SessionObserverRegistry& observers, common::Logger& logger,
            ResumeSeed seed = {});

    // Terminates and joins. A Session is never destroyed with its worker running.
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Reader thread. Queues a turn and wakes the worker; never blocks. False
    // means the turn was REFUSED -- terminated, a finished subagent, or the
    // work queue is full -- and an error event answering the refusal has been
    // pushed onto the outbound queue, so a client is never left waiting on a
    // turn that was silently dropped.
    //
    // Also commands::SessionControl::SubmitTurn -- the one sanctioned path for
    // a prompt-shorthand command's content to reach the model (EXTENSIBILITY
    // 4.4). A shorthand runs on the worker, so its SubmitTurn queues behind
    // itself and runs at the next loop iteration.
    bool SubmitTurn(std::string text) override;

    // SessionControl: SubmitTurn carrying PER-TURN overrides. The model is
    // re-rendered to its raw "<provider>/<model>" form and everything is
    // split/resolved at pop, on the worker; the session's sticky choices are
    // untouched. Refusal contract identical to SubmitTurn.
    bool SubmitTurnWithOverrides(std::string text, const std::optional<common::ModelRef>& model,
                                 const std::string& agent_preset) override;

    // The queue-level form the wire and the control path both converge on:
    // raw ref strings, resolution entirely at pop. Exposed for tests.
    bool SubmitTurnWithOverrides(QueuedTurn turn);

    // Reader thread. Queues a MUTATING command for the worker, which runs it at
    // a turn boundary -- never between a tool_use and its tool_result, because
    // the worker only pops between RunTurn calls. Read-only commands do not come
    // through here; they run on the RPC layer's command pool instead (see
    // RunCommandNow). False means refused, with the same error-event guarantee
    // as SubmitTurn.
    bool SubmitCommand(QueuedCommand command);

    // Answers a command with an error CommandResult without running it. Used
    // when the command pool is saturated -- the caller holds a request_id a
    // client is waiting on, so refusal must still produce an answer.
    void RejectCommand(const QueuedCommand& command, const std::string& reason);

    // Runs a command immediately on the CALLING thread. For read-only commands
    // only, dispatched by the RPC layer onto its bounded command pool -- never
    // inline on the reader, which must not block.
    void RunCommandNow(const QueuedCommand& command);

    // Reader thread. Queues a rewind/redo for the worker, which runs it at a
    // turn boundary -- the same structural guarantee a mutating command has:
    // never between a tool_use and its tool_result. False means refused
    // (terminated, a subagent session, a malformed request, or the work queue
    // is full), and an error event answering the refusal has been pushed
    // where the queue can still carry one -- a client is never left waiting
    // on a rewind that was silently dropped.
    bool SubmitRewind(QueuedRewind request);

    // Reader thread. Queues a permission run-mode change (shift+tab) for the
    // worker, which applies it at a turn boundary -- the same
    // worker-thread-only contract SetRunMode has. False means refused
    // (terminated, or the work queue is full); a SessionError event answers
    // the refusal on the wire, so the press is never silently dropped.
    bool SubmitRunMode(QueuedRunMode request);
    // Cross-session splice point: a non-blocking subagent's worker queues its
    // completion here when it finishes; the session's own worker flushes it at
    // a turn boundary. False means the session is already terminating and the
    // message was NOT queued -- the caller (the spawner) logs the loss rather
    // than reporting a completion nobody will ever see.
    bool QueueInjectedMessage(llm::Message message);

    // Reader thread. Interrupts the in-flight turn, leaving the session alive
    // and able to accept the next one.
    void Cancel();

    // Stops the worker and closes the outbound queue. Idempotent, and safe to
    // call with a turn in flight -- that is its main use. Every terminate path
    // runs through here, which is what makes the terminal wire event
    // (`SessionEndedEvent`, queued just before the close) fire exactly once on
    // every way a session can die.
    void Terminate();

    // Publishes a SessionEvent through the sink every producer inside this
    // class already uses internally; the one entry point external callers
    // (SubagentManager, announcing a spawned child on the PARENT's stream) get
    // too, so nothing outside Session ever needs to reach the transport
    // directly. Returns what the sink returned -- see SessionEventSink's
    // comment for what the bool means.
    bool EmitEvent(const SessionEvent& event) { return event_sink_(event); }

    // The opaque handle to this session's rpc::EventTranslator, for callers
    // that must reach the real transport (SessionServiceImpl's
    // Subscribe/Unsubscribe/Resolve/Pop). Never null once construction
    // succeeds -- SessionParams::transport_handle is required.
    std::shared_ptr<void> transport() const { return transport_; }

    const SessionInfo& info() const { return info_; }
    // Locked copy of info(), for any reader OFF the session worker that needs
    // a field SetName or ApplyPreset can mutate (name, agent_preset) -- an
    // unlocked info() read of those fields races the writer's std::string
    // assignment. info() itself remains valid for the truly immutable fields
    // (session_id, parent_session_id, project_root) and for worker-side
    // callers that already synchronize with the writers.
    SessionInfo info_snapshot() const { return InfoSnapshot(); }

    // The titler path (a SessionObserver plugin, reached through
    // SessionSupportService::RenameSession): updates the in-memory name and
    // Publishes a SessionMetadataEvent so an attached client refreshes its tab
    // label. The DATABASE write belongs to the caller -- the service does
    // rename-then-notify, keeping Session free of store concerns it does not
    // otherwise have. Publish never blocks (the titler runs on a detached
    // thread); after Terminate the event is simply lost -- a dropped metadata
    // refresh is display-only, the persisted name is the authoritative record.
    //
    // Safe from any thread. This is the ONLY mutation of info_.name after
    // construction, and it synchronizes on mutex_; ApplyPreset similarly
    // updates info_.agent_preset under that same mutex. Unlocked readers must
    // use info_snapshot(); info() stays a plain const-ref for the truly
    // immutable fields (session_id, parent_session_id, project_root).
    void SetName(std::string name);

    // The provider this session is currently running, shared (instances are
    // stateless by contract). The ONLY writer is ApplyPreset on the worker
    // between turns, and that swap now takes mutex_ too, so a titler thread can
    // snapshot it safely. Null only in plain test sessions that never had one.
    std::shared_ptr<llm::LlmProvider> ProviderSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return provider_;
    }

    // The session's transcript read side. Bound at construction and never
    // mutated, so the shared_ptr copy is safe from any thread. Null when the
    // session is not SQLite-backed (plain tests).
    std::shared_ptr<const session::SessionReader> history_reader() const {
        return history_reader_;
    }

    // --- commands::SessionControl ------------------------------------------
    // Worker-thread only, which is what makes the mutating half lock-free: a
    // command that touches this is not ReadOnly(), so it runs on the worker
    // between turns -- the same thread that reads it while running one.
    const std::string& session_id() const override { return params_.session_id; }
    // Immutable for the session's lifetime (SessionParams::project_root): no
    // lock, and safe from any thread.
    std::filesystem::path project_root() const override { return params_.project_root; }
    AgentLoop& loop() override { return *loop_; }
    tools::ToolExecutor& executor() override { return executor_; }
    // WORKER-THREAD ONLY: a reference into state the worker rewrites in
    // ApplyPreset. A read-only command runs on the RPC layer's pool, not the
    // worker, and must use preset_snapshot() instead.
    const AgentPreset& preset() const override { return preset_; }
    // Safe from any thread: a copy taken under the session lock. This is what
    // read-only commands (running off the worker) read.
    AgentPreset preset_snapshot() const override;
    void ApplyPreset(AgentPreset preset) override;

    // Session-start and /agent liveness resolution against the McpClientManager:
    // for every server in the preset's allowed_mcp_servers, EnsureAlive; a
    // failure disables the server FOR THIS SESSION (recorded, surfaced as an
    // ErrorEvent) and the session proceeds without it. Sets the loop's live
    // server list. Runs where blocking is allowed -- session construction and
    // the worker at a preset-switch boundary -- never the RPC reader.
    void RefreshMcpServers();
    const std::optional<common::ModelRef>& model_override() const override {
        return model_override_;
    }
    void set_model_override(std::optional<common::ModelRef> ref) override {
        model_override_ = std::move(ref);
        // Durable record of the decision, best-effort: the in-memory state is
        // what this session runs on; the hook is what a later resume reads.
        if (params_.model_override_persist) params_.model_override_persist(model_override_);
    }



    // The registry the /agent command re-resolves against. Borrowed; it
    // outlives every session. Constructor-injected from SessionParams --
    // see that struct's "constructor-injected wiring" section.
    const AgentPresetRegistry* presets() const { return presets_; }
    // What a spawner reads off a PARENT at spawn time to derive the child's
    // inheritance: permission ceilings, the run mode to clamp against, the
    // grants to copy, and the preset whose allowed lists the child intersects.
    // preset() is the SessionControl override above -- one accessor, since the
    // spawner and a /-command want exactly the same thing.
    const tools::PermissionPolicy& policy() const { return executor_.policy(); }
    const tools::McpPermissionPolicy& mcp_policy() const { return executor_.mcp_policy(); }
    tools::PermissionRunMode run_mode() const { return executor_.run_mode(); }
    std::vector<std::string> granted_keys() const { return executor_.granted_keys(); }

    // The initial-attach guard reads these. A session that has never been
    // attached is reaped after a short grace period -- purely a safety net
    // against a non-conforming client, since attaching is a client protocol
    // obligation (see ARCHITECTURE.md "Session lifetime").
    //
    // S5: N STREAMS MAY ATTACH, not one. SessionServiceImpl owns the actual
    // Subscribe/Unsubscribe against the transport (via transport()) now that
    // Session no longer names rpc::EventBroadcaster -- these two calls are
    // its obligation to Session in exchange: bracket every subscribe/
    // unsubscribe so ever_attached()/has_subscribers() keep meaning what they
    // always meant. The reaper's detached-idle rule reads has_subscribers();
    // it deliberately replaced the old single-stream `attached_` flag, which
    // existed only because one queue had one consumer.
    void NotifyAttached() {
        ever_attached_.store(true, std::memory_order_release);
        subscriber_count_.fetch_add(1, std::memory_order_acq_rel);
    }
    void NotifyDetached() { subscriber_count_.fetch_sub(1, std::memory_order_acq_rel); }
    bool has_subscribers() const {
        return subscriber_count_.load(std::memory_order_acquire) > 0;
    }

    // Live background tasks (background bash whose STARTED event has not yet
    // been followed by a terminal one). The reaper's quiescence rule reads
    // this: reaping a session with running background work would kill that
    // work. Conservative failure mode -- a watcher that dies without emitting
    // a terminal event leaks the count UPWARD, i.e. toward "never reap".
    // The counter itself lives on the channels block (SessionChannels), which
    // is the object the maintaining sink outlives.
    std::size_t background_tasks_live() const {
        return channels_->background_tasks_live.load(std::memory_order_acquire);
    }

    bool ever_attached() const { return ever_attached_.load(std::memory_order_acquire); }
    std::chrono::steady_clock::time_point created_at() const { return created_at_; }

    bool terminated() const { return terminated_.load(std::memory_order_acquire); }
    // A subagent whose single seeded turn has ended. Distinct from terminated:
    // a finished child stays in the registry until its client's stream closes,
    // and the two states mean different things to different readers -- the
    // concurrency limit counts the finished child as FREE (its turn no longer
    // runs), while SubmitTurn reads it as CLOSED (the completion hook must
    // fire exactly once).
    bool subagent_finished() const {
        return subagent_finished_.load(std::memory_order_acquire);
    }
    bool idle() const;

    // Testing seam: blocks until the turn queue is empty and the worker is idle.
    // Production code never waits on a session -- the client is driven by the
    // event stream, not by turn completion.
    bool WaitForIdleForTesting(std::chrono::milliseconds timeout);

    // Testing seam: pins the background-task count the way a live background
    // bash would (a STARTED with no terminal event yet), without running one.
    // The reaper's quiescence rule and the relaxed process idle exit both read
    // this counter; production never sets it directly.
    void SetBackgroundTasksForTesting(std::size_t live) {
        channels_->background_tasks_live.store(live, std::memory_order_release);
    }

private:
    void WorkerMain();
    // info_ copied under mutex_ -- the snapshot the Notify* call sites take, so
    // a concurrent SetName can never race a payload copy.
    SessionInfo InfoSnapshot() const;
    // nullopt = the wake path: a turn whose user content is the injected
    // messages themselves (see SessionParams::wake_on_injection).
    void RunOneTurn(std::optional<std::string> text, const QueuedTurn* overrides = nullptr);
    void RunOneCommand(const QueuedCommand& command);
    void RunOneRewind(const QueuedRewind& request);
    void RunOneRunMode(const QueuedRunMode& request);
    // The full E-additions metadata set (name, preset, model, run mode,
    // context window) as one SessionMetadataEvent. Called after anything
    // mutates what it carries -- ApplyPreset, a run-mode switch -- so every
    // attached client's status bar refreshes without a re-attach. Worker
    // thread and the command pool are both safe: EmitEvent's sink never
    // blocks.
    void EmitMetadataChanged();
    void RunRewind(std::int64_t target_seq);
    void RunRedo();
    // Answers a rewind with an ErrorEvent. Publish never blocks on any path;
    // the only refusal is a broadcaster closed by termination.
    void EmitRewindRefusal(const std::string& reason);
    // The success event: what the active transcript now ends at, so the client
    // can trim its scrollback and clear streaming state.
    void EmitSessionRewound(std::int64_t truncated_to_seq, std::int64_t active_last_seq);
    // Builds the context and dispatches. Shared by the worker path and the
    // read-only pool path so the two cannot diverge on what a command may reach.
    void ExecuteCommand(const QueuedCommand& command, bool busy);
    // Publishes a CommandResult. The sink never blocks, so the RPC pool and
    // the worker are equally safe callers.
    void EmitCommandResult(commands::CommandResult result, const QueuedCommand& command);
    // The turn terminator. Emitted on EVERY exit path, including failures --
    // a client that learns a turn ended only by seeing it succeed hangs on
    // every error.
    void EmitTurnCompleted(llm::StopReason stop_reason, const llm::Usage& usage,
                           bool hit_max_turns, bool usage_estimated = false);
    // A brand-new session is unnamed until its first user turn's text names it:
    // `candidate` (agent::DefaultSessionName of the turn's raw text, computed
    // by RunOneTurn before the turn runs) is stored via
    // HistoryStore::SetDefaultNameIfUnset -- the domain rule (WHICH text
    // counts, how it is shaped) lives here, not in SessionDatabase; the store
    // only guards "still unnamed" and persists. Only when the store confirms
    // this call is the one that set it does info_ update and a
    // SessionMetadataEvent go out -- the wire source the TUI's tab label
    // refreshes from. Worker thread only.
    void EmitDefaultNameIfComputed(const std::string& candidate);
    bool IsCancelled() const;
    // The worker's wait predicate extension: should an out-of-band message
    // wake this parked worker and drive a seedless turn? Called under mutex_.
    bool ShouldWakeForInjections() const;
    // The only call to worker_.join(), reached from both branches of
    // Terminate(). std::call_once makes a second, concurrent caller BLOCK
    // here rather than also calling join() -- concurrent join() on one
    // std::thread is UB. The self-join check stays at each call site (not
    // folded in here): a self-terminating worker must still be able to run
    // this without blocking on itself, and only an external caller's call
    // should ever perform or wait on the real join.
    void JoinWorkerOnce();

    SessionParams params_;
    SessionInfo info_;
    AgentPreset preset_;
    common::Logger& logger_;
    SessionObserverRegistry& observers_;

    // shared_ptr, not unique_ptr: with ProviderInstanceCache the instance is
    // SHARED across sessions, so the session holds a reference rather than
    // ownership. Stateless-by-contract providers are freely shareable.
    std::shared_ptr<llm::LlmProvider> provider_;
    std::unique_ptr<session::HistoryStore> store_;

    // Track M: the read side of the durable transcript and the tool-facing
    // window over it (docs/EXTENSIBILITY.md §2.1). Declared BEFORE loop_ so
    // destruction order is safe: loop_ is destroyed first, and it is what
    // borrows history_window_ -- the borrowed pointer never outlives the
    // window. Null pair when the store is not SQLite-backed (plain tests).
    std::shared_ptr<const session::SessionReader> history_reader_;
    std::unique_ptr<session::HistoryWindow> history_window_;

    // Domain-only cross-thread state; always Session's own (nothing external
    // ever needs to share it since the transport half split out).
    std::shared_ptr<SessionChannels> channels_;
    // The permission decision source: always producer-supplied now (the wire
    // default, an advised checker, or a test fake) -- see
    // SessionParams::permission_checker.
    std::unique_ptr<tools::PermissionChecker> checker_;
    std::unique_ptr<commands::ConfirmationBroker> confirmations_;
    // May be null (see SessionParams::ask_broker).
    std::unique_ptr<tools::AskBroker> ask_broker_;
    // Where every SessionEvent this class emits goes. See EmitEvent() and
    // SessionParams::event_sink.
    SessionEventSink event_sink_;
    // Opaque; see transport() and SessionParams::transport_handle.
    std::shared_ptr<void> transport_;
    std::function<void()> shutdown_pending_requests_;
    std::function<void()> cancel_pending_requests_;
    std::function<void()> close_transport_;
    // Mirrors the transport's own subscriber count (Session cannot query it
    // directly any more) so has_subscribers()/ever_attached() keep working off
    // NotifyAttached()/NotifyDetached() calls from whoever owns Subscribe/
    // Unsubscribe now (SessionServiceImpl).
    std::atomic<std::size_t> subscriber_count_{0};

    tools::ToolExecutor executor_;
    std::unique_ptr<AgentLoop> loop_;

    // The command surface. Borrowed: both outlive every session.
    const commands::CommandRegistry* commands_ = nullptr;
    const AgentPresetRegistry* presets_ = nullptr;
    const llm::ProviderFactoryRegistry* provider_factories_ = nullptr;
    ProviderInstanceCache* provider_instance_cache_ = nullptr;
    mcp::McpClientManager* mcp_manager_ = nullptr;
    // The process-wide hook dispatcher (track O), read from loop_config.hooks
    // in the constructor -- one field, not two (Phase 5 step 8):
    // LoopConfig::hooks already drives AgentLoop's compaction emissions, so
    // Session reads the same copy for kUserPromptSubmit and the ToolExecutor
    // it builds, instead of also taking its own SessionParams::hooks.
    const hooks::HookDispatcher* hooks_ = nullptr;
    // The user's explicit model choice. Outranks the preset's, and survives
    // /agent re-resolving it.
    std::optional<common::ModelRef> model_override_;

    // Two signals, because they mean different things: cancel ends the CURRENT
    // turn and is inert for the next one; terminated ends the session.
    //
    // Cancel is a GENERATION, not a flag, because a flag cannot express "this
    // turn, not the next one" without a race in either clearing direction:
    // clearing after the pop loses a Cancel that landed in the pop-to-clear
    // window; clearing before the pop loses one that landed before the worker
    // got there, which then runs a full turn the user cancelled. Instead the
    // worker records the generation at pop time (turn_cancel_base_) and a turn
    // is cancelled iff the generation has moved since. A Cancel for a turn that
    // has already finished lands on the NEXT turn only if the user cancels
    // again -- which is exactly what "cancel ends the current turn" means.
    std::atomic<std::uint64_t> cancel_gen_{0};
    // Written by the worker when it pops a turn; read by IsCancelled on the
    // worker's own call stack (loop, sink, tools, compactor).
    std::atomic<std::uint64_t> turn_cancel_base_{0};
    // The event-driven face of the same mechanism: the worker mints a token
    // per turn (recorded in the agent loop, handed to every ToolContext), and
    // Cancel/Terminate fire every live one, so a tool blocked on a condition
    // variable wakes immediately instead of polling. See tools/cancel_token.hpp.
    tools::CancelSource cancel_source_;
    // The advisor cancel holder's flag, when an advisor is composed for this
    // session: shared_ptr, so Cancel/Terminate can fire it and the DETACHED
    // advise thread can still be polling the same atomic after this Session is
    // destroyed. Null without an advisor. See AdvisorCancelSource.
    std::shared_ptr<std::atomic<bool>> advisor_cancel_flag_;
    std::atomic<bool> terminated_{false};
    // A subagent's life is one seeded turn; once its completion hook has fired,
    // a stray client-submitted turn would fire it a second time, so SubmitTurn
    // stops accepting. Only ever set when on_turn_finished exists.
    std::atomic<bool> subagent_finished_{false};
    std::atomic<bool> ever_attached_{false};
    std::chrono::steady_clock::time_point created_at_ = std::chrono::steady_clock::now();

    mutable std::mutex mutex_;
    std::condition_variable turn_cv_;
    std::deque<WorkItem> work_;
    bool running_turn_ = false;

    // The work queue is BOUNDED, unlike the outbound EventQueue's backpressure
    // there is nothing pulling from here at a predictable rate: one turn can run
    // for minutes, so a client streaming turns could otherwise enqueue them
    // faster than they are consumed and grow memory without limit. The bound is
    // a ceiling against a runaway producer, not a tuning knob; beyond it the
    // submit is refused with an error event rather than dropped silently.
    static constexpr std::size_t kMaxWorkItems = 32;

    std::thread worker_;
    std::once_flag join_once_;
};

}  // namespace minimal::agent
