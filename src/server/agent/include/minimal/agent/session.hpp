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
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/rpc/event_queue.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/wire_confirmation_broker.hpp"
#include "minimal/rpc/wire_permission_checker.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::agent {

struct SessionParams {
    std::string session_id;
    std::string parent_session_id;  // empty for a top-level session
    std::string name;
    std::string agent_preset;
    std::filesystem::path project_root;
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
    // session.
    Session(SessionParams params, AgentPreset preset, std::unique_ptr<llm::LlmProvider> provider,
            std::unique_ptr<HistoryStore> store, tools::ToolRegistry& registry,
            LoopConfig loop_config, SessionObserverRegistry& observers, common::Logger& logger,
            std::vector<llm::Message> seeded_history = {});

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
    // a turn boundary.
    void QueueInjectedMessage(llm::Message message);

    // Reader thread. Interrupts the in-flight turn, leaving the session alive
    // and able to accept the next one.
    void Cancel();

    // Stops the worker and closes the outbound queue. Idempotent, and safe to
    // call with a turn in flight -- that is its main use.
    void Terminate();

    // Drained by the writer thread; the sole Write() source.
    rpc::EventQueue<v1::ServerEvent>& queue() { return queue_; }
    // Resolved from the reader thread when a Permission/Confirmation response
    // arrives.
    rpc::PendingRequests& pending() { return pending_; }

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

    std::unique_ptr<llm::LlmProvider> provider_;
    std::unique_ptr<HistoryStore> store_;

    rpc::EventQueue<v1::ServerEvent> queue_;
    rpc::PendingRequests pending_;
    rpc::WirePermissionChecker checker_;
    rpc::WireConfirmationBroker confirmations_;

    tools::ToolExecutor executor_;
    std::unique_ptr<AgentLoop> loop_;

    // The command surface. Borrowed: both outlive every session.
    const commands::CommandRegistry* commands_ = nullptr;
    const AgentPresetRegistry* presets_ = nullptr;
    const llm::ProviderFactoryRegistry* provider_factories_ = nullptr;
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
