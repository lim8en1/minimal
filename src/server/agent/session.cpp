#include "minimal/agent/session.hpp"

#include <algorithm>

#include "minimal/mcp/mcp_client_manager.hpp"

#include "minimal/agent/provider_instance_cache.hpp"

#include <exception>
#include <utility>

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
                 std::vector<llm::Message> seeded_history,
                 std::optional<std::int64_t> seed_first_persisted_seq, bool seed_head_is_summary)
    : params_(std::move(params)),
      preset_(std::move(preset)),
      logger_(logger),
      observers_(observers),
      provider_(std::move(provider)),
      store_(std::move(store)),
      // Track M: the registry injects the reader when the transcript is
      // SQLite-backed; plain tests on fake stores leave it null and tools see
      // ctx.history == nullptr. The window is built here, bound to THIS
      // session's id -- a subagent's window is bound to the child's id the
      // same way, which is what isolates the child's transcript from the
      // parent's (docs/EXTENSIBILITY.md §2.1).
      history_reader_(std::move(params_.history_reader)),
      history_window_(history_reader_
                          ? std::make_unique<session::HistoryWindow>(*history_reader_,
                                                                     params_.session_id)
                          : nullptr),
      // The channels block comes from the producer when it composed a checker
      // that had to be built against the queue and pending maps (an advised
      // permission checker, docs/EXTENSIBILITY.md §3); otherwise Session makes
      // its own, which is what every plain session and test does.
      channels_(params_.channels ? std::move(params_.channels) : MakeSessionChannels()),
      checker_(params_.permission_checker ? std::move(params_.permission_checker)
                                          : std::make_unique<rpc::WirePermissionChecker>(
                                                params_.session_id, channels_->queue,
                                                channels_->pending)),
      confirmations_(params_.session_id, channels_->queue, channels_->pending),
      // The run mode the client asked for wins over the configured default: it
      // is a property of the CLIENT, not of the config. A headless run has no
      // human to answer a prompt, so it must be able to say so. apply_run_mode()
      // is applied downstream, once, by ToolExecutor. For a subagent the
      // spawner has already clamped the override against the parent's run mode,
      // and hands down the parent's policies as ceilings and the parent's
      // grants as seed -- the whole inheritance lands in this one call.
      executor_(registry, *checker_, preset_.permission_mode, preset_.mcp_permissions,
                params_.run_mode_override.value_or(preset_.run_mode), params_.policy_ceiling,
                params_.mcp_policy_ceiling, params_.seed_grants) {
    // An advised checker was composed before this Session existed, so only its
    // shared cancel holder could not yet be bound to a session: install the
    // predicate now. The worker starts at the end of this constructor, so there
    // is no window in which an advisor sees the default "never cancelled".
    if (params_.advisor_cancel_source) {
        params_.advisor_cancel_source->is_cancelled = [this] { return IsCancelled(); };
    }

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
    // ToolContext::history for every tool this session runs; null when there
    // is no SQLite-backed reader behind it.
    context.history = history_window_.get();

    const std::string session_id = params_.session_id;
    // Sinks that OUTLIVE their invocation (background bash's watcher captures
    // them past the tool call, the turn, and this worker's lifetime) must be
    // backed by the channels shared_ptr, never by `this` or a raw queue
    // pointer: after Terminate, Push/TryPush into the closed queue refuse
    // harmlessly instead of dangling. The worker-only sinks below capture raw
    // pointers deliberately -- they cannot be invoked after the worker joins.
    const auto channels = channels_;
    auto* queue = &channels_->queue;

    context.injections = std::shared_ptr<InjectionQueue>(channels, &channels_->injections);
    context.session_lifetime = channels_;

    context.tool_output_sink = [channels, session_id](std::string_view tool_use_id,
                                                      std::string_view chunk) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* delta = event.mutable_tool_output_delta();
        delta->set_tool_use_id(std::string(tool_use_id));
        delta->set_chunk(std::string(chunk));
        // Push, not TryPush: the common caller is the worker (sync bash), which
        // is allowed to block. The background watcher may block here too when
        // the queue is full -- a stalled client backpressures it the same way,
        // and Terminate's Close wakes it. See the background-task watcher.
        channels->queue.Push(std::move(event));
    };

    context.task_event_sink = [channels, session_id](const tools::BackgroundTaskEvent& task) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* message = event.mutable_background_task();
        message->set_task_id(task.task_id);
        message->set_tool_use_id(task.tool_use_id);
        message->set_command(task.command);
        message->set_pid(task.pid);
        switch (task.state) {
            case tools::BackgroundTaskEvent::State::kStarted:
                message->set_state(v1::BackgroundTaskEvent::STARTED);
                break;
            case tools::BackgroundTaskEvent::State::kCompleted:
                message->set_state(v1::BackgroundTaskEvent::COMPLETED);
                break;
            case tools::BackgroundTaskEvent::State::kFailed:
                message->set_state(v1::BackgroundTaskEvent::FAILED);
                break;
            case tools::BackgroundTaskEvent::State::kTimedOut:
                message->set_state(v1::BackgroundTaskEvent::TIMED_OUT);
                break;
            case tools::BackgroundTaskEvent::State::kKilled:
                message->set_state(v1::BackgroundTaskEvent::KILLED);
                break;
        }
        // The caller may be the watcher thread, never the worker: TryPush. A
        // full queue means the client is stalled; losing one lifecycle event
        // is the correct degradation (the authoritative record is the injected
        // message and the persisted transcript, not this status channel).
        if (!channels->queue.TryPush(std::move(event))) {
            // DefaultLogger is process-lifetime; the session logger is the
            // same object. This lambda can outlive `logger_`'s session, but
            // not the logger itself.
            common::DefaultLogger().warn("session {}: background task event for '{}' dropped "
                                         "(queue full)",
                                         session_id, task.task_id);
        }
    };

    context.tool_lifecycle_sink = [this, queue, session_id](const ToolLifecycleEvent& lifecycle) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* execution = event.mutable_tool_execution();
        execution->set_tool_use_id(lifecycle.tool_use_id);
        execution->set_tool_name(lifecycle.tool_name);
        *execution->mutable_input() = rpc::JsonToStruct(lifecycle.input);
        execution->set_started(lifecycle.started);
        if (!lifecycle.started) execution->set_is_error(lifecycle.is_error);
        execution->set_started_at_unix_ms(lifecycle.started_at_unix_ms);
        queue->Push(std::move(event));

        // AND THE OBSERVER SIDE, which had no producer at all. ARCHITECTURE.md
        // documents OnToolExecuted as one of the four SessionObserver hooks, and
        // NotifyToolExecuted's only caller in the whole tree was its own unit
        // test -- so a plugin implementing the documented hook got silence,
        // which is worse than the hook not existing.
        //
        // On the FINISH event only: a started/finished pair is the wire's
        // concern (a client has a panel to clear), whereas an observer wants one
        // record of a completed call. Fired for a FAILED tool too, for the same
        // reason the wire event is.
        if (lifecycle.started) return;
        ToolInvocation invocation;
        invocation.tool_use_id = lifecycle.tool_use_id;
        invocation.tool_name = lifecycle.tool_name;
        invocation.is_error = lifecycle.is_error;
        invocation.duration = lifecycle.duration;
        // `summary` stays empty deliberately: the only honest summary is the
        // tool's own disclosure, which is built for a permission prompt and is
        // not reached when the call is pre-approved. An observer that wants
        // detail has SessionReader and the tool_use_id.
        observers_.NotifyToolExecuted(info_, std::move(invocation));
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
    // loaded from. The seq/head companions carry where that seed's persisted
    // portion begins, which is what compaction checkpoints are written against.
    loop_ = std::make_unique<AgentLoop>(*provider_, executor_, registry, *store_, preset_,
                                         loop_config, std::move(context),
                                         [this] { return IsCancelled(); }, std::move(event_sink),
                                         compact_messages, std::move(seeded_history),
                                         seed_first_persisted_seq, seed_head_is_summary);

    // MCP liveness at session start, BEFORE the worker exists: this thread is
    // allowed to block (the RPC reader is a different one), and the loop must
    // already carry its final server set when the first turn can run.
    mcp_manager_ = params_.mcp_manager;
    if (mcp_manager_ != nullptr) RefreshMcpServers();

    observers_.NotifySessionCreated(info_);
    worker_ = std::thread([this] { WorkerMain(); });
}

void Session::RefreshMcpServers() {
    std::vector<std::string> live;
    live.reserve(preset_.allowed_mcp_servers.size());
    for (const auto& server : preset_.allowed_mcp_servers) {
        // Once failed, disabled for this session's life: re-testing on every
        // /agent switch would make an operator's typo a per-switch stall, and
        // the surfaced error already told the client what happened.
        if (std::find(disabled_mcp_servers_.begin(), disabled_mcp_servers_.end(), server) !=
            disabled_mcp_servers_.end()) {
            continue;
        }
        std::string error;
        if (mcp_manager_->EnsureAlive(server, error)) {
            live.push_back(server);
            continue;
        }
        disabled_mcp_servers_.push_back(server);
        // Surfaced, not swallowed: an ErrorEvent on the outbound queue is the
        // same channel a failed turn uses, so the client sees WHY the
        // server's tools never appeared.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* err = event.mutable_error();
        err->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        err->set_message(error);
        err->set_is_retryable(false);
        channels_->queue.Push(std::move(event));
        logger_.warn("session {}: MCP server '{}' disabled: {}", params_.session_id, server,
                     error);
    }
    loop_->SetMcpServers(std::move(live));
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
        if (!channels_->queue.TryPush(std::move(event))) {
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
    if (!channels_->queue.TryPush(std::move(event))) {
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
    // The new preset may allow different MCP servers. Runs on the worker at a
    // turn boundary (the same blocking contract as a preset swap): a dead
    // server in the new list is surfaced here, never discovered mid-turn.
    if (mcp_manager_ != nullptr) RefreshMcpServers();

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
    if (!channels_->queue.TryPush(std::move(event))) {
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
    // The wake decision is the worker's, not ours: its wait predicate sees the
    // pending message (ShouldWakeForInjections) and runs a seedless turn when
    // params_.wake_on_injection says so. But a predicate change WAKES NOBODY
    // by itself -- the parked worker must be notified, or it sleeps with its
    // predicate already true until some unrelated event lands. The empty
    // critical section closes the lost-wakeup window: a worker either saw
    // (work empty, injections pending) before parking, or was not yet parked
    // and cannot miss the notify because it must acquire mutex_ to park.
    const bool staged = channels_->injections.Push(std::move(message));
    if (staged) {
        { std::lock_guard<std::mutex> lock(mutex_); }
        turn_cv_.notify_all();
    }
    return staged;
}

void Session::Cancel() {
    // Bump the generation: every turn popped BEFORE this point observes a
    // cancelled() that is now true; a turn popped after records the new
    // generation as its base and runs uncancelled. One mechanism, no
    // clear-timing race in either direction.
    cancel_gen_.fetch_add(1, std::memory_order_acq_rel);
    // Fire the turn's token: a tool parked on its cv (MCP call) wakes now,
    // not on its next poll slice. Everything a live token's waiters do from
    // here is the same path a deadline takes -- a cancelled tool call.
    cancel_source_.CancelAll();
    // Wakes a worker parked on a permission prompt. Without this, cancelling
    // during an Ask would set a flag nobody is in a position to read.
    //
    // CancelAll, not Shutdown: this ends the TURN. The distinction is
    // load-bearing -- a permanent cancel here leaves every later permission
    // prompt in the session answering "no answer" instantly, which
    // WirePermissionChecker turns into Deny, so one CancelTurn would silently
    // deny-lock a session that reports itself alive and keeps accepting turns.
    channels_->pending.CancelAll();
}

void Session::Terminate() {
    if (terminated_.exchange(true, std::memory_order_acq_rel)) {
        // Already terminating. Still join, so that a second caller does not
        // return while the worker is alive -- the destructor relies on it.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
        return;
    }

    // NOT a cancel-generation bump: Terminate ends the SESSION, and IsCancelled
    // already folds terminated_ in. It DOES fire the cancel tokens, though --
    // a tool blocked on a token cv must wake to unwind. Shutting pending
    // requests is the other half of the same wakeup: it releases a worker
    // parked on a gate mid-teardown -- including one parked in
    // WirePermissionChecker::check on a question the client will never answer
    // (the other half of the two-file dependency documented at that Push).
    cancel_source_.CancelAll();
    channels_->pending.Shutdown();
    // Refuses future out-of-band pushes: a background task finishing mid-
    // teardown learns "session gone" from its schedule_injection returning
    // false, exactly like a subagent's completion hook. Already-staged
    // messages stay drainable but nobody will drain them -- the worker is
    // exiting, and a session nobody is attached to loses its in-flight
    // injections by design.
    channels_->injections.Close();
    turn_cv_.notify_all();

    // Close BEFORE the join, and the order is the whole point. Push() blocks at
    // the queue's cap and is woken only by Close() or Pop(), so a worker parked
    // on a full queue -- reachable whenever the writer stalls or the client dies
    // mid-stream -- would deadlock a join that waits for it. Closing first wakes
    // that producer; the drain-before-close rule in Pop() still delivers every
    // already-queued event to a writer that is still attached. The only events
    // lost are ones the dying turn pushes after this point, for a session whose
    // consumer is going away regardless.
    channels_->queue.Close();

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
        bool wake_turn = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            turn_cv_.wait(lock, [this] {
                return !work_.empty() || terminated_.load(std::memory_order_acquire) ||
                       ShouldWakeForInjections();
            });
            if (terminated_.load(std::memory_order_acquire)) return;
            // Record the cancel generation THIS turn must be measured against,
            // under the same lock as the pop. A Cancel before this point
            // belongs to the turn that already ended (the worker is between
            // turns); one after it targets the turn about to run. See the
            // member comment for why the generation exists at all.
            turn_cancel_base_.store(cancel_gen_.load(std::memory_order_acquire),
                                    std::memory_order_relaxed);
            // The turn's cancellation token: the loop hands it to every
            // ToolContext, so a tool blocked on a condition variable (MCP)
            // wakes the moment Cancel or Terminate fires. Minted AFTER the
            // generation snapshot, exactly like the base above: a token from
            // the previous turn stays fired for its own stragglers, and this
            // turn starts clean.
            loop_->SetCancelToken(cancel_source_.Mint());
            if (!work_.empty()) {
                item = std::move(work_.front());
                work_.pop_front();
            } else {
                // Only reachable when ShouldWakeForInjections() fired: an
                // out-of-band message landed on an idle session and the wake
                // setting is on. A seedless turn whose user content is the
                // injected messages themselves.
                wake_turn = true;
            }
            running_turn_ = true;
        }

        // The turn-boundary guarantee, and it is structural rather than checked:
        // this pops one item at a time and RunOneTurn is atomic from here, so a
        // command can only ever land BETWEEN turns -- never between a tool_use
        // and its tool_result.
        if (wake_turn) {
            RunOneTurn(std::nullopt);
        } else if (auto* text = std::get_if<std::string>(&item)) {
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

bool Session::ShouldWakeForInjections() const {
    // Called from the wait predicate, under mutex_. Lock order is one-way:
    // mutex_ -> the queue's own mutex; producers (subagent completion hooks,
    // background watchers) hold only the queue's mutex, so no cycle exists.
    if (!params_.wake_on_injection) return false;
    if (running_turn_ || !work_.empty()) return false;
    // A finished subagent's life is over regardless of what is staged.
    if (subagent_finished_.load(std::memory_order_acquire)) return false;
    return !channels_->injections.empty();
}

void Session::RunOneTurn(std::optional<std::string> text) {
    const auto started = std::chrono::steady_clock::now();

    TurnResult result;
    std::int64_t first_seq = 0;
    try {
        first_seq = store_->last_appended_seq() + 1;
        if (text) {
            result =
                loop_->RunTurn(llm::Message{llm::Role::User, {llm::TextBlock{std::move(*text)}}});
        } else {
            // The wake path: no seed. The turn's user content is whatever the
            // loop flushes at turn start -- the injected messages themselves.
            result = loop_->RunTurn();
        }
    } catch (const llm::LlmError& error) {
        // The loop rethrows once retries are exhausted. The session survives:
        // one failed turn is not a reason to tear down a conversation the user
        // can retry.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        *event.mutable_error() = rpc::ToProto(error);
        channels_->queue.Push(std::move(event));
        logger_.warn("session {}: turn failed: {}", params_.session_id, error.what());
    } catch (const std::exception& e) {
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* error = event.mutable_error();
        error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        error->set_message(e.what());
        error->set_is_retryable(false);
        channels_->queue.Push(std::move(event));
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
        channels_->queue.Push(std::move(event));
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
        // Specified by ARCHITECTURE.md since track I and shipped permanently
        // empty, because the loop collected it nowhere and this never set it.
        summary.tool_names_invoked = std::move(result.tool_names_invoked);
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
        // EXCHANGE, not store. Terminate() guards the same hook with an
        // exchange and its comment claims that makes the pair exactly-once --
        // which was true only because Terminate joins this worker before reading
        // the flag, i.e. because of the JOIN, not because of the exchange. A
        // claim resting on a mechanism other than the one it names is how the
        // next edit breaks it; with both sides exchanging, exactly-once holds
        // whatever the ordering. Firing twice would set a std::promise twice,
        // which throws.
        if (params_.on_turn_finished &&
            !subagent_finished_.exchange(true, std::memory_order_acq_rel)) {
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
    channels_->queue.Push(std::move(event));
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
