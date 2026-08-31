#include "minimal/agent/session.hpp"

#include "minimal/agent/provider_instance_cache.hpp"

#include <exception>
#include <utility>

#include "minimal/agent/mention_expansion.hpp"
#include "minimal/rpc/proto_convert.hpp"

namespace minimal::agent {
namespace {

v1::ServerEvent MakeEvent(const std::string& session_id) {
    v1::ServerEvent event;
    event.set_session_id(session_id);
    return event;
}

}  // namespace

Session::Session(SessionParams params, AgentPreset preset,
                 std::shared_ptr<llm::LlmProvider> provider, std::unique_ptr<HistoryStore> store,
                 tools::ToolRegistry& registry, LoopConfig loop_config,
                 SessionObserverRegistry& observers, common::Logger& logger,
                 std::vector<llm::Message> seeded_history)
    : params_(std::move(params)),
      preset_(std::move(preset)),
      logger_(logger),
      observers_(observers),
      provider_(std::move(provider)),
      store_(std::move(store)),
      checker_(params_.session_id, queue_, pending_),
      confirmations_(params_.session_id, queue_, pending_),
      // The run mode the client asked for wins over the configured default: it
      // is a property of the CLIENT, not of the config. A headless run has no
      // human to answer a prompt, so it must be able to say so. apply_run_mode()
      // is applied downstream, once, by ToolExecutor. For a subagent the
      // spawner has already clamped the override against the parent's run mode,
      // and hands down the parent's policies as ceilings and the parent's
      // grants as seed -- the whole inheritance lands in this one call.
      executor_(registry, checker_, preset_.permission_mode, preset_.mcp_permissions,
                params_.run_mode_override.value_or(preset_.run_mode), params_.policy_ceiling,
                params_.mcp_policy_ceiling, params_.seed_grants) {
    // Seeded from what the client asked for. THE REGISTRY already applied it to
    // the preset before construction, but applying is not the same as
    // remembering: /agent re-resolves the preset from disk, and without the
    // choice recorded HERE it would silently revert to the preset's own model.
    model_override_ = params_.model_override;

    info_.session_id = params_.session_id;
    info_.parent_session_id = params_.parent_session_id;
    info_.name = params_.name;
    info_.agent_preset = params_.agent_preset;
    info_.project_root = params_.project_root;

    SessionContext context{logger_};
    context.session_id = params_.session_id;
    context.project_root = params_.project_root;

    const std::string session_id = params_.session_id;
    auto* queue = &queue_;

    context.tool_output_sink = [queue, session_id](std::string_view tool_use_id,
                                                   std::string_view chunk) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* delta = event.mutable_tool_output_delta();
        delta->set_tool_use_id(std::string(tool_use_id));
        delta->set_chunk(std::string(chunk));
        queue->Push(std::move(event));
    };

    context.tool_lifecycle_sink = [queue, session_id](const ToolLifecycleEvent& lifecycle) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* execution = event.mutable_tool_execution();
        execution->set_tool_use_id(lifecycle.tool_use_id);
        execution->set_tool_name(lifecycle.tool_name);
        *execution->mutable_input() = rpc::JsonToStruct(lifecycle.input);
        execution->set_started(lifecycle.started);
        if (!lifecycle.started) execution->set_is_error(lifecycle.is_error);
        execution->set_started_at_unix_ms(lifecycle.started_at_unix_ms);
        queue->Push(std::move(event));
    };

    // Returning false aborts the provider request. This is the cancellation path
    // that termination reuses -- there is no second mechanism, and adding one is
    // how a session ends up with two half-working ways to stop.
    llm::StreamEventSink event_sink = [this, queue, session_id](const llm::StreamEvent& stream) {
        if (IsCancelled()) return false;
        v1::ServerEvent event = MakeEvent(session_id);
        *event.mutable_stream_event() = rpc::ToProto(stream);
        queue->Push(std::move(event));
        return true;
    };

    // The real summarizer. Passed as the plain function rather than wrapped,
    // because everything it needs -- provider, model, prompt, cancel flag -- is
    // an argument the loop supplies per call. That is what keeps a mid-session
    // /agent switch from compacting with the previous preset's prompt.
    //
    // seeded_history is non-empty only on resume: the stored transcript becomes
    // the loop's working history WITHOUT being re-appended to the store it was
    // loaded from.
    loop_ = std::make_unique<AgentLoop>(*provider_, executor_, registry, *store_, preset_,
                                         loop_config, std::move(context),
                                         [this] { return IsCancelled(); }, std::move(event_sink),
                                         compact_messages, std::move(seeded_history));

    observers_.NotifySessionCreated(info_);
    worker_ = std::thread([this] { WorkerMain(); });
}

Session::~Session() { Terminate(); }

bool Session::IsCancelled() const {
    // A turn is cancelled iff the cancel generation moved past the base recorded
    // when this turn was popped, or the session is terminating. See the member
    // comment for why this is a generation rather than a flag.
    return cancel_gen_.load(std::memory_order_acquire) !=
               turn_cancel_base_.load(std::memory_order_relaxed) ||
           terminated_.load(std::memory_order_acquire);
}

bool Session::SubmitTurn(std::string text) {
    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return false;
        // A subagent's life is ONE turn: once it has finished, a further turn
        // would run a session its spawner has already collected and reported on.
        if (subagent_finished_.load(std::memory_order_acquire)) return false;
        if (work_.size() >= kMaxWorkItems) {
            refused = true;
        } else {
            work_.push_back(std::move(text));
        }
    }
    if (refused) {
        // Answer the refusal rather than dropping it: the client sent a turn and
        // must learn it was not accepted. TryPush because this runs on the
        // reader, which must never block even on its own error path.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* error = event.mutable_error();
        error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        error->set_message("session backlog is full; turn refused");
        error->set_is_retryable(false);
        if (!queue_.TryPush(std::move(event))) {
            logger_.warn("session {}: turn refused and the queue is full; client will not see it",
                         params_.session_id);
        }
        return false;
    }
    turn_cv_.notify_one();
    return true;
}

bool Session::SubmitCommand(QueuedCommand command) {
    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return false;
        // The SAME queue as turns, deliberately. Two queues would have no order
        // between them, so `/model x` typed before a turn could apply after it.
        if (work_.size() >= kMaxWorkItems) {
            refused = true;
        } else {
            work_.push_back(std::move(command));
        }
    }
    if (refused) {
        // Same contract as SubmitTurn, plus the request_id: a client waiting on
        // a CommandResult must not hang because its command was not accepted.
        RejectCommand(QueuedCommand{std::move(command)}, "session backlog is full");
        return false;
    }
    turn_cv_.notify_one();
    return true;
}

void Session::RejectCommand(const QueuedCommand& command, const std::string& reason) {
    v1::ServerEvent event = MakeEvent(params_.session_id);
    auto* result = event.mutable_command_result();
    result->set_request_id(command.request_id);
    result->set_status(v1::COMMAND_STATUS_ERROR);
    result->set_message(reason);
    if (!queue_.TryPush(std::move(event))) {
        logger_.warn("session {}: command '{}' refused and the queue is full; client will not "
                     "see it",
                     params_.session_id, command.name);
    }
}

AgentPreset Session::preset_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return preset_;
}

void Session::RunCommandNow(const QueuedCommand& command) {
    // Read-only only, and the caller guarantees that. Runs on the RPC layer's
    // bounded command pool -- never the reader, which must not block, and never
    // the worker, which must not be preempted mid-turn.
    ExecuteCommand(command, /*busy=*/false);
}

void Session::ApplyPreset(AgentPreset preset) {
    // THE PROVIDER MAY HAVE CHANGED, and commands must not have to think about
    // it: /agent to a preset on another backend, or /model across providers,
    // both land here. Detected by comparing what actually identifies a
    // connection rather than by asking the caller to say so.
    const bool provider_changed = preset.provider_kind != preset_.provider_kind ||
                                  preset.provider.base_url != preset_.provider.base_url ||
                                  preset.provider.api_key != preset_.provider.api_key;

    // THREE copies of the preset exist -- this one, the loop's, and the
    // executor's run mode derived from it. A command updating only some is a bug
    // that surfaces a turn later, so there is one entry point and it updates all
    // of them.
    //
    // The swap takes mutex_ because preset_snapshot() reads preset_ from the
    // RPC layer's command pool, off the worker. The rest of this method touches
    // only worker-visible state and stays lock-free.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preset_ = std::move(preset);
    }
    executor_.SetRunMode(params_.run_mode_override.value_or(preset_.run_mode));
    loop_->SetPreset(preset_);

    if (!provider_changed) return;
    if (provider_factories_ == nullptr) {
        logger_.warn("session {}: cannot rebuild the provider; keeping the previous one",
                     params_.session_id);
        return;
    }

    // Through the instance cache, not a fresh construct: entries are immutable
    // at runtime, so a switch back to an entry this process already built is a
    // cache hit -- the same instance, shared with any session using it. The
    // PREVIOUS provider needs no teardown either: this session only drops its
    // reference, and the cache holds the instance for whoever else uses it.
    std::shared_ptr<llm::LlmProvider> rebuilt;
    if (provider_instance_cache_ != nullptr) {
        rebuilt = provider_instance_cache_->GetOrCreate(preset_.model_ref.provider,
                                                        preset_.provider_kind, preset_.provider);
    } else {
        rebuilt = std::shared_ptr<llm::LlmProvider>(
            provider_factories_->Create(preset_.provider_kind, preset_.provider));
    }
    if (rebuilt == nullptr) {
        // The preset resolved, so the kind was known at that point; failing to
        // construct is still possible and must not leave the loop holding a
        // dangling reference. Keeping the old provider is wrong but survivable;
        // dropping it is not.
        logger_.warn("session {}: provider kind '{}' failed to construct; keeping the previous one",
                     params_.session_id, preset_.provider_kind);
        return;
    }

    // Worker thread, between turns: the loop is provably not inside a provider
    // call, so rebinding and then releasing the old reference is safe. Rebind
    // FIRST -- the loop must never hold a reference to the object being freed.
    auto previous = std::move(provider_);
    provider_ = std::move(rebuilt);
    loop_->SetProvider(*provider_);
}

void Session::ExecuteCommand(const QueuedCommand& command, bool busy) {
    v1::ServerEvent event = MakeEvent(params_.session_id);
    auto* result = event.mutable_command_result();
    result->set_request_id(command.request_id);

    if (commands_ == nullptr || presets_ == nullptr) {
        // No registry wired: answer rather than drop, or a client waiting on a
        // CommandResult hangs on a request nobody will ever handle.
        result->set_status(v1::COMMAND_STATUS_ERROR);
        result->set_message("the /-command subsystem is not available in this session");
        EmitCommandResult(std::move(event), command);
        return;
    }

    commands::CommandContext context(*this, *presets_, confirmations_, logger_);
    const auto outcome = commands::Dispatch(*commands_, command.name, command.args, context, busy);

    result->set_status(outcome.status == commands::CommandResult::Status::kOk
                           ? v1::COMMAND_STATUS_OK
                           : v1::COMMAND_STATUS_ERROR);
    result->set_message(outcome.message);
    EmitCommandResult(std::move(event), command);
}

void Session::EmitCommandResult(v1::ServerEvent event, const QueuedCommand& command) {
    // TryPush, never Push: ExecuteCommand runs on the worker AND on the RPC
    // layer's command pool, and only the worker is allowed to block. A full
    // queue means the client is stalled or gone; losing one command answer is
    // the correct degradation there, logged so it is not silent.
    if (!queue_.TryPush(std::move(event))) {
        logger_.warn("session {}: queue is full; CommandResult for '{}' dropped",
                     params_.session_id, command.name);
    }
}

void Session::RunOneCommand(const QueuedCommand& command) {
    // Reached only from the worker, and only between turns -- so `busy` is
    // false by construction here too. The parameter exists for the reader-side
    // refusal path, which checks before queueing.
    ExecuteCommand(command, /*busy=*/false);
}

bool Session::QueueInjectedMessage(llm::Message message) {
    if (terminated_.load(std::memory_order_acquire)) return false;
    loop_->QueueInjectedMessage(std::move(message));
    return true;
}

void Session::Cancel() {
    // Bump the generation: every turn popped BEFORE this point observes a
    // cancelled() that is now true; a turn popped after records the new
    // generation as its base and runs uncancelled. One mechanism, no
    // clear-timing race in either direction.
    cancel_gen_.fetch_add(1, std::memory_order_acq_rel);
    // Wakes a worker parked on a permission prompt. Without this, cancelling
    // during an Ask would set a flag nobody is in a position to read.
    //
    // CancelAll, not Shutdown: this ends the TURN. The distinction is
    // load-bearing -- a permanent cancel here leaves every later permission
    // prompt in the session answering "no answer" instantly, which
    // WirePermissionChecker turns into Deny, so one CancelTurn would silently
    // deny-lock a session that reports itself alive and keeps accepting turns.
    pending_.CancelAll();
}

void Session::Terminate() {
    if (terminated_.exchange(true, std::memory_order_acq_rel)) {
        // Already terminating. Still join, so that a second caller does not
        // return while the worker is alive -- the destructor relies on it.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
        return;
    }

    // NOT a cancel-generation bump: Terminate ends the SESSION, and IsCancelled
    // already folds terminated_ in. Shutting pending requests is what wakes a
    // worker parked on a gate mid-teardown.
    pending_.Shutdown();
    turn_cv_.notify_all();

    // Close BEFORE the join, and the order is the whole point. Push() blocks at
    // the queue's cap and is woken only by Close() or Pop(), so a worker parked
    // on a full queue -- reachable whenever the writer stalls or the client dies
    // mid-stream -- would deadlock a join that waits for it. Closing first wakes
    // that producer; the drain-before-close rule in Pop() still delivers every
    // already-queued event to a writer that is still attached. The only events
    // lost are ones the dying turn pushes after this point, for a session whose
    // consumer is going away regardless.
    queue_.Close();

    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();

    // The subagent completion hook fires on EVERY exit path -- including a
    // terminate that lands before the seeded turn ever ran (the initial-attach
    // reaper, or a terminate racing SubmitTurn). Without this, a blocking parent
    // waits on a promise that nothing will ever set. The exchange makes
    // exactly-once certain against the store in RunOneTurn.
    if (params_.on_turn_finished && !subagent_finished_.exchange(true, std::memory_order_acq_rel)) {
        TurnResult cancelled;
        cancelled.stop_reason = llm::StopReason::Cancelled;
        try {
            params_.on_turn_finished(cancelled);
        } catch (...) {
            logger_.warn("session {}: on_turn_finished threw during teardown", params_.session_id);
        }
    }

    observers_.NotifySessionEnded(info_);
}

void Session::WorkerMain() {
    while (true) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            turn_cv_.wait(lock, [this] {
                return !work_.empty() || terminated_.load(std::memory_order_acquire);
            });
            if (terminated_.load(std::memory_order_acquire)) return;
            // Record the cancel generation THIS turn must be measured against,
            // under the same lock as the pop. A Cancel before this point
            // belongs to the turn that already ended (the worker is between
            // turns); one after it targets the turn about to run. See the
            // member comment for why the generation exists at all.
            turn_cancel_base_.store(cancel_gen_.load(std::memory_order_acquire),
                                    std::memory_order_relaxed);
            item = std::move(work_.front());
            work_.pop_front();
            running_turn_ = true;
        }

        // The turn-boundary guarantee, and it is structural rather than checked:
        // this pops one item at a time and RunOneTurn is atomic from here, so a
        // command can only ever land BETWEEN turns -- never between a tool_use
        // and its tool_result.
        if (auto* text = std::get_if<std::string>(&item)) {
            RunOneTurn(std::move(*text));
        } else {
            RunOneCommand(std::get<QueuedCommand>(item));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_turn_ = false;
        }
        turn_cv_.notify_all();
    }
}

void Session::RunOneTurn(std::string text) {
    const auto started = std::chrono::steady_clock::now();

    // @-mention expansion is for the HUMAN's typed turns only: a subagent's
    // task arrives from the model, and expanding '@' in model-generated prose
    // would misfire on emails and ordinary sentences. No mentions -> a message
    // byte-identical to the plain wrap (mention_expansion.hpp), so the
    // unconditional-looking ternary changes nothing for anyone else.
    llm::Message seed = params_.parent_session_id.empty()
                            ? ExpandMentions(text, params_.project_root, logger_)
                            : llm::Message{llm::Role::User, {llm::TextBlock{std::move(text)}}};

    TurnResult result;
    std::int64_t first_seq = 0;
    try {
        first_seq = store_->last_appended_seq() + 1;
        result = loop_->RunTurn(std::move(seed));
    } catch (const llm::LlmError& error) {
        // The loop rethrows once retries are exhausted. The session survives:
        // one failed turn is not a reason to tear down a conversation the user
        // can retry.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        *event.mutable_error() = rpc::ToProto(error);
        queue_.Push(std::move(event));
        logger_.warn("session {}: turn failed: {}", params_.session_id, error.what());
    } catch (const std::exception& e) {
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* error = event.mutable_error();
        error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        error->set_message(e.what());
        error->set_is_retryable(false);
        queue_.Push(std::move(event));
        logger_.warn("session {}: turn threw: {}", params_.session_id, e.what());
    } catch (...) {
        // Neither an LlmError nor a std::exception. The catches above existed to
        // keep one failed turn from taking down the worker -- and with it the
        // process -- but anything else escaping here would still have reached
        // std::terminate through WorkerMain. Same treatment: one degraded turn,
        // a live session.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* error = event.mutable_error();
        error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        error->set_message("the turn threw an unrecognized exception");
        error->set_is_retryable(false);
        queue_.Push(std::move(event));
        logger_.warn("session {}: turn threw a non-standard exception", params_.session_id);
    }

    // BOTH endings run through here, and that is the point. A failed turn leaves
    // `result` default-constructed -- StopReason::Unknown, zero usage -- which is
    // exactly what a failure should report.
    //
    // The wire event is emitted on every exit path because a client that learns
    // a turn ended only by seeing it succeed hangs on every error. The observer
    // notification is the same obligation on the in-process side: an observer
    // holding per-turn state has no other signal that the turn is over, so
    // returning early from a catch block leaked it once per failure. Same
    // argument as emitting tool-finish even when the tool errored.
    //
    // Each stage is guarded SEPARATELY: the bookkeeping after the turn is as
    // fallible as the turn itself (the seq read touches SQLite, observers are
    // plugin code), and one stage throwing must not skip the later ones -- a
    // blocking subagent waiting on on_turn_finished must unblock even if the
    // observer notify above it just failed.
    try {
        EmitTurnCompleted(result.stop_reason, result.last_usage, result.hit_max_turns);
    } catch (...) {
        logger_.warn("session {}: emitting TurnCompletedEvent failed", params_.session_id);
    }
    try {
        TurnSummary summary;
        summary.first_seq = first_seq;
        summary.last_seq = store_->last_appended_seq();
        summary.stop_reason = result.stop_reason;
        summary.usage = result.last_usage;
        summary.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        observers_.NotifyTurnCompleted(info_, std::move(summary));
    } catch (...) {
        logger_.warn("session {}: notifying turn observers failed", params_.session_id);
    }
    try {
        // Last, after every wire event and observer notification: a blocking
        // parent waiting on this result may proceed the instant the callback
        // returns, and must not observe a session that still has bookkeeping in
        // flight. Fired on EVERY exit path -- `result` is default-constructed
        // after a failed turn, which is exactly what a waiter needs to unblock
        // with an error.
        if (params_.on_turn_finished) {
            subagent_finished_.store(true, std::memory_order_release);
            params_.on_turn_finished(result);
        }
    } catch (...) {
        logger_.warn("session {}: on_turn_finished threw", params_.session_id);
    }
}

void Session::EmitTurnCompleted(llm::StopReason stop_reason, const llm::Usage& usage,
                                bool hit_max_turns) {
    v1::ServerEvent event = MakeEvent(params_.session_id);
    auto* completed = event.mutable_turn_completed();
    completed->set_stop_reason(rpc::ToProto(stop_reason));
    *completed->mutable_usage() = rpc::ToProto(usage);
    completed->set_hit_max_turns(hit_max_turns);
    queue_.Push(std::move(event));
}

bool Session::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return work_.empty() && !running_turn_;
}

bool Session::WaitForIdleForTesting(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return turn_cv_.wait_for(lock, timeout, [this] {
        return (work_.empty() && !running_turn_) || terminated_.load(std::memory_order_acquire);
    });
}

}  // namespace minimal::agent
