#pragma once

// One open session: the agent loop, its tools, its provider, its transcript, and
// the worker thread all of that runs on.
//
// THREE THREADS PER SESSION, and the split is the reason this class exists
// rather than the loop being driven from the RPC handler (ARCHITECTURE.md
// "Concurrency model"):
//
//   Reader  -- the gRPC handler thread. Never blocks on anything but Read().
//              Calls SubmitTurn / Cancel / pending().Resolve and returns.
//   Worker  -- owned here. Blocks freely: permission waits, provider calls,
//              bash subprocesses.
//   Writer  -- owned by the transport, drains queue() and is the sole Write()
//              caller.
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
#include "minimal/agent/history_store.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_dispatcher.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/rpc/event_queue.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/wire_confirmation_broker.hpp"
#include "minimal/rpc/wire_permission_checker.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::mcp {
class McpClientManager;       // borrowed via SessionParams::mcp_manager
}  // namespace minimal::mcp

namespace minimal::agent {

class ProviderInstanceCache;  // borrowed via set_provider_instance_cache

// The session's two transport-side primitives, extracted from Session into a
// shareable block: the params PRODUCER creates them so it can also build a
// WirePermissionChecker (or a decoration over one) before the Session exists --
// the seam the permission advisor composes through (docs/EXTENSIBILITY.md §3).
// Session owns the block through a shared_ptr; the queue and pending maps are
// exactly the members Session used to hold, unchanged in behavior.
struct SessionChannels {
    rpc::EventQueue<v1::ServerEvent> queue;
    rpc::PendingRequests pending;
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
    // The transport-side primitives this session uses. Producers create one via
    // MakeSessionChannels() whenever they also build a checker (below); null
    // means Session creates its own -- the path every plain test takes.
    std::shared_ptr<SessionChannels> channels;
    // The permission decision source for this session. Null means the wire
    // checker (the default -- prompts round-trip to the client). A producer
    // that configured an advisor composes AdvisedPermissionChecker(wire) and
    // injects it; a test injects any fake. ToolExecutor sees one checker
    // either way.
    std::unique_ptr<tools::PermissionChecker> permission_checker;
    // Set only when permission_checker is an AdvisedPermissionChecker composed
    // around this shared holder: Session installs its own cancel predicate into
    // it in the constructor body, before the worker starts. Null otherwise.
    std::shared_ptr<tools::AdvisorCancelSource> advisor_cancel_source;
    // Set when the client asked for a specific mode (the headless client asks
    // for AllowOnly); unset means the preset's configured mode stands. For a
    // subagent the spawner has ALREADY clamped it against the parent's via
    // clamp_run_mode -- a child is never more permissive than its parent.
    std::optional<tools::PermissionRunMode> run_mode_override;
    // Set when the user named a model explicitly (--model, later /model). Wins
    // over whatever the preset resolves to, and is re-applied after a preset
    // switch -- the preset is where a default comes from, not a decision.
    std::optional<config::ModelRef> model_override;
    // True when this Session reopens a STORED session: the registry loads the
    // stored transcript as the loop's seeded history (without re-appending it)
    // before construction. The session row must already exist either way --
    // the database mints ids -- but a fresh session's transcript is empty.
    bool resume = false;

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

    // The process-wide hook dispatcher (track O). Borrowed; null in tests that
    // do not exercise hooks, which makes every emission a no-op. Set by the
    // registry -- the one funnel every session (top-level and subagent alike)
    // passes through -- so producers need not know it exists; a producer MAY
    // set it explicitly (a test with its own dispatcher wins over the funnel
    // default, mirroring how params.permission_checker behaves).
    const hooks::HookDispatcher* hooks = nullptr;
};

// One item of work for the session worker. A variant rather than two queues,
// because the ORDER between a turn and a command matters and two queues have no
// order between them -- `/model x` typed before a turn must apply before it.
struct QueuedCommand {
    std::string request_id;  // echoed on CommandResult
    std::string name;
    commands::CommandArgs args;
};
using WorkItem = std::variant<std::string, QueuedCommand>;  // string = a user turn

class Session : public commands::SessionControl {
public:
    // `provider` is owned because a session outlives any one turn and the
    // provider is stateless by contract; `registry` and `observers` are
    // process-wide and outlive every session.
    // `provider` and `store` are owned; `registry` and `observers` are
    // process-wide. The store is injected rather than constructed here so a
    // session does not have to know whether its transcript is durable -- and so
    // tests can drive one without a database file.
    // `seeded_history` is the RESUME path: the stored transcript, loaded
    // without being re-appended (the store already holds it). Empty for a new
    // session. The seq/head companions tell the loop where the seed's persisted
    // portion begins -- what compaction checkpoints are written against (see
    // AgentLoop's constructor for the invariant).
    Session(SessionParams params, AgentPreset preset, std::shared_ptr<llm::LlmProvider> provider,
            std::unique_ptr<HistoryStore> store, tools::ToolRegistry& registry,
            LoopConfig loop_config, SessionObserverRegistry& observers, common::Logger& logger,
            std::vector<llm::Message> seeded_history = {},
            std::optional<std::int64_t> seed_first_persisted_seq = std::nullopt,
            bool seed_head_is_summary = false);

    // Terminates and joins. A Session is never destroyed with its worker running.
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Reader thread. Queues a turn and wakes the worker; never blocks. False
    // means the turn was REFUSED -- terminated, a finished subagent, or the
    // work queue is full -- and an error event answering the refusal has been
    // pushed onto the outbound queue, so a client is never left waiting on a
    // turn that was silently dropped.
    bool SubmitTurn(std::string text);

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
    // call with a turn in flight -- that is its main use.
    void Terminate();

    // Drained by the writer thread; the sole Write() source.
    rpc::EventQueue<v1::ServerEvent>& queue() { return channels_->queue; }
    // Resolved from the reader thread when a Permission/Confirmation response
    // arrives.
    rpc::PendingRequests& pending() { return channels_->pending; }

    const SessionInfo& info() const { return info_; }

    // --- commands::SessionControl ------------------------------------------
    // Worker-thread only, which is what makes the mutating half lock-free: a
    // command that touches this is not ReadOnly(), so it runs on the worker
    // between turns -- the same thread that reads it while running one.
    const std::string& session_id() const override { return params_.session_id; }
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
    }



    // The registry the /agent command re-resolves against. Borrowed; it outlives
    // every session.
    const AgentPresetRegistry* presets() const { return presets_; }
    void set_presets(const AgentPresetRegistry* presets) { presets_ = presets; }
    void set_commands(const commands::CommandRegistry* commands) { commands_ = commands; }
    // Needed to rebuild the provider when a preset or model switch crosses
    // provider entries. Borrowed; process-wide.
    void set_provider_factories(const llm::ProviderFactoryRegistry* factories) {
        provider_factories_ = factories;
    }
    // The shared instance cache the rebuild above routes through. Optional:
    // without it the rebuild constructs a private instance (test sessions).
    void set_mcp_manager(mcp::McpClientManager* manager) { mcp_manager_ = manager; }
    void set_provider_instance_cache(ProviderInstanceCache* cache) {
        provider_instance_cache_ = cache;
    }
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
    // obligation (see ARCHITECTURE.md "Session lifetime"). Once attached, the
    // guard no longer applies: stream drop is what terminates from then on.
    // Claims the session for one stream. False means someone is already
    // attached, and the caller must refuse rather than attach anyway: the
    // outbound queue has ONE consumer by design, so a second writer draining it
    // would give each stream a random half of the events -- and the first stream
    // to close would terminate the session under the other. Exchange rather than
    // a store so the check and the claim cannot race.
    bool TryAttach() {
        ever_attached_.store(true, std::memory_order_release);
        return !attached_.exchange(true, std::memory_order_acq_rel);
    }
    void Detach() { attached_.store(false, std::memory_order_release); }

    bool ever_attached() const { return ever_attached_.load(std::memory_order_acquire); }
    std::chrono::steady_clock::time_point created_at() const { return created_at_; }

    bool terminated() const { return terminated_.load(std::memory_order_acquire); }
    bool idle() const;

    // Testing seam: blocks until the turn queue is empty and the worker is idle.
    // Production code never waits on a session -- the client is driven by the
    // event stream, not by turn completion.
    bool WaitForIdleForTesting(std::chrono::milliseconds timeout);

private:
    void WorkerMain();
    void RunOneTurn(std::string text);
    void RunOneCommand(const QueuedCommand& command);
    // Builds the context and dispatches. Shared by the worker path and the
    // read-only pool path so the two cannot diverge on what a command may reach.
    void ExecuteCommand(const QueuedCommand& command, bool busy);
    // Publishes a CommandResult. TryPush: ExecuteCommand also runs on the RPC
    // layer's pool, and only the worker may block.
    void EmitCommandResult(v1::ServerEvent event, const QueuedCommand& command);
    // The turn terminator. Emitted on EVERY exit path, including failures --
    // a client that learns a turn ended only by seeing it succeed hangs on
    // every error.
    void EmitTurnCompleted(llm::StopReason stop_reason, const llm::Usage& usage,
                           bool hit_max_turns);
    bool IsCancelled() const;

    SessionParams params_;
    SessionInfo info_;
    AgentPreset preset_;
    common::Logger& logger_;
    SessionObserverRegistry& observers_;

    // shared_ptr, not unique_ptr: with ProviderInstanceCache the instance is
    // SHARED across sessions, so the session holds a reference rather than
    // ownership. Stateless-by-contract providers are freely shareable.
    std::shared_ptr<llm::LlmProvider> provider_;
    std::unique_ptr<HistoryStore> store_;

    // Track M: the read side of the durable transcript and the tool-facing
    // window over it (docs/EXTENSIBILITY.md §2.1). Declared BEFORE loop_ so
    // destruction order is safe: loop_ is destroyed first, and it is what
    // borrows history_window_ -- the borrowed pointer never outlives the
    // window. Null pair when the store is not SQLite-backed (plain tests).
    std::shared_ptr<const session::SessionReader> history_reader_;
    std::unique_ptr<session::HistoryWindow> history_window_;

    // The transport-side primitives, created here or shared in via params (see
    // SessionParams::channels).
    std::shared_ptr<SessionChannels> channels_;
    // The permission decision source: the wire default built here, or the
    // producer's composition (an advised checker, a test fake) moved in.
    std::unique_ptr<tools::PermissionChecker> checker_;
    rpc::WireConfirmationBroker confirmations_;

    tools::ToolExecutor executor_;
    std::unique_ptr<AgentLoop> loop_;

    // The command surface. Borrowed: both outlive every session.
    const commands::CommandRegistry* commands_ = nullptr;
    const AgentPresetRegistry* presets_ = nullptr;
    const llm::ProviderFactoryRegistry* provider_factories_ = nullptr;
    ProviderInstanceCache* provider_instance_cache_ = nullptr;
    mcp::McpClientManager* mcp_manager_ = nullptr;
    // Servers that failed a liveness check in THIS session. Disabled for the
    // session's life: the session proceeds without them (ARCHITECTURE.md, MCP
    // client), and later /agent switches do not re-test them.
    std::vector<std::string> disabled_mcp_servers_;
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
    std::atomic<bool> terminated_{false};
    // A subagent's life is one seeded turn; once its completion hook has fired,
    // a stray client-submitted turn would fire it a second time, so SubmitTurn
    // stops accepting. Only ever set when on_turn_finished exists.
    std::atomic<bool> subagent_finished_{false};
    std::atomic<bool> ever_attached_{false};
    // Currently attached, as distinct from ever attached: the guard above is a
    // one-way latch for the reaper, this one goes back down on detach.
    std::atomic<bool> attached_{false};
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
};

}  // namespace minimal::agent
