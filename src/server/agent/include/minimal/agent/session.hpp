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
    // for AllowOnly); unset means the preset's configured mode stands.
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

    // Reader thread. Queues a turn and wakes the worker; never blocks.
    void SubmitTurn(std::string text);

    // Reader thread. Queues a MUTATING command for the worker, which runs it at
    // a turn boundary -- never between a tool_use and its tool_result, because
    // the worker only pops between RunTurn calls. Read-only commands do not come
    // through here; they run on a short-lived task instead (see RunCommandNow).
    void SubmitCommand(QueuedCommand command);

    // Runs a command immediately on the CALLING thread. For read-only commands
    // only, dispatched by the RPC layer onto a short-lived task -- never inline
    // on the reader, which must not block.
    void RunCommandNow(const QueuedCommand& command);

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
    const AgentPreset& preset() const override { return preset_; }
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
    // read-only task path so the two cannot diverge on what a command may reach.
    void ExecuteCommand(const QueuedCommand& command, bool busy);
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

    // Two flags, because they mean different things: cancel_ ends the current
    // turn and is cleared for the next one; terminated_ ends the session.
    std::atomic<bool> cancel_{false};
    std::atomic<bool> terminated_{false};
    std::atomic<bool> ever_attached_{false};
    // Currently attached, as distinct from ever attached: the guard above is a
    // one-way latch for the reaper, this one goes back down on detach.
    std::atomic<bool> attached_{false};
    std::chrono::steady_clock::time_point created_at_ = std::chrono::steady_clock::now();

    mutable std::mutex mutex_;
    std::condition_variable turn_cv_;
    std::deque<WorkItem> work_;
    bool running_turn_ = false;

    std::thread worker_;
};

}  // namespace minimal::agent
