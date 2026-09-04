#include "minimal/agent/session.hpp"

#include <algorithm>

#include "minimal/mcp/mcp_client_manager.hpp"

#include "minimal/agent/provider_instance_cache.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "minimal/agent/mention_expansion.hpp"

namespace minimal::agent {
namespace {

// Fails construction loudly, before any member is built, rather than letting
// a missing wiring field surface later as a silently-dropped event or a
// std::bad_function_call from deep inside a lambda. Every real session gets
// these from rpc::FinishSessionWiring (SessionRegistry::CreateResolved); a
// test that constructs a Session directly must call it (or
// rpc::ApplyPermissionAdvisor + rpc::FinishSessionWiring) itself first.
SessionParams&& RequireWiring(SessionParams&& params) {
    if (!params.event_sink) {
        throw std::invalid_argument("SessionParams::event_sink is required");
    }
    if (!params.permission_checker) {
        throw std::invalid_argument("SessionParams::permission_checker is required");
    }
    if (!params.confirmation_broker) {
        throw std::invalid_argument("SessionParams::confirmation_broker is required");
    }
    if (!params.transport_handle) {
        throw std::invalid_argument("SessionParams::transport_handle is required");
    }
    if (!params.cancel_pending_requests || !params.shutdown_pending_requests ||
        !params.close_transport) {
        throw std::invalid_argument(
            "SessionParams::cancel_pending_requests/shutdown_pending_requests/close_transport are "
            "required");
    }
    return std::move(params);
}

}  // namespace

Session::Session(SessionParams params, AgentPreset preset,
                 std::shared_ptr<llm::LlmProvider> provider, std::unique_ptr<session::HistoryStore> store,
                 tools::ToolRegistry& registry, LoopConfig loop_config,
                 SessionObserverRegistry& observers, common::Logger& logger, ResumeSeed seed)
    : params_(RequireWiring(std::move(params))),
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
      // Domain-only cross-thread state (background task bookkeeping,
      // injections) -- always Session's own now that the transport half
      // (queue, pending) lives on the producer-supplied wiring instead.
      channels_(MakeSessionChannels()),
      // Every field below is producer-supplied -- see RequireWiring above and
      // rpc::FinishSessionWiring for who builds the wire defaults.
      checker_(std::move(params_.permission_checker)),
      confirmations_(std::move(params_.confirmation_broker)),
      event_sink_(std::move(params_.event_sink)),
      transport_(std::move(params_.transport_handle)),
      shutdown_pending_requests_(std::move(params_.shutdown_pending_requests)),
      cancel_pending_requests_(std::move(params_.cancel_pending_requests)),
      close_transport_(std::move(params_.close_transport)),
      // The run mode the client asked for wins over the configured default: it
      // is a property of the CLIENT, not of the config. A headless run has no
      // human to answer a prompt, so it must be able to say so. apply_run_mode()
      // is applied downstream, once, by ToolExecutor. For a subagent the
      // spawner has already clamped the override against the parent's run mode,
      // and hands down the parent's policies as ceilings and the parent's
      // grants as seed -- the whole inheritance lands in this one call.
      executor_(registry, *checker_, preset_.permission_mode, preset_.mcp_permissions,
                params_.run_mode_override.value_or(preset_.run_mode), params_.policy_ceiling,
                params_.mcp_policy_ceiling, params_.seed_grants, loop_config.hooks) {
    // An advised checker was composed before this Session existed. The
    // producer shares the cancel HOLDER between the checker and these params;
    // the Session's only job is to keep the flag and fire it on Cancel/
    // Terminate. The predicate the advisor polls is the holder's own -- it
    // reads the shared atomic and captures nothing owned by this object --
    // because the advise thread is DETACHED on budget expiry and keeps
    // polling after this Session is destroyed. An earlier draft installed
    // `[this] { return IsCancelled(); }`: the shared_ptr kept the holder
    // alive, not the Session, so every post-teardown poll was a
    // use-after-free. The worker starts at the end of this constructor, so
    // there is no window in which an advisor sees a pre-Session flag.
    if (params_.advisor_cancel_source) {
        advisor_cancel_flag_ = params_.advisor_cancel_source->cancelled;
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
    // The domain channels block (background task bookkeeping, injections)
    // still needs sharing with sinks that OUTLIVE their invocation --
    // background bash's watcher captures it past the tool call, the turn,
    // and this worker's lifetime. event_sink_ needs no such care: every copy
    // of it is independently self-sufficient (it is backed by an
    // rpc::EventTranslator, whose own copy of the transport shared_ptr keeps
    // the broadcaster alive on its own), so any sink below may capture it by
    // value regardless of which thread ends up calling it.
    const auto channels = channels_;

    context.injections = std::shared_ptr<InjectionQueue>(channels, &channels_->injections);
    context.session_lifetime = channels_;

    context.tool_output_sink = [sink = event_sink_](std::string_view tool_use_id,
                                                    std::string_view chunk) {
        // The sink never blocks (event_broadcaster.hpp): a slow or absent
        // subscriber drops its oldest and resyncs, and no subscriber at all
        // buffers in the backlog. The turn never waits on a client.
        sink(SessionEvent{ToolOutputDelta{std::string(tool_use_id), std::string(chunk)}});
    };

    context.task_event_sink = [channels, sink = event_sink_,
                               session_id](const tools::BackgroundTaskEvent& task) {
        auto& live_tasks = channels->background_tasks_live;
        // Quiescence bookkeeping for the reaper (see background_tasks_live).
        // STARTED increments; every terminal state decrements -- floored at
        // zero, so a duplicated terminal event cannot wrap the count downward.
        if (task.state == tools::BackgroundTaskEvent::State::kStarted) {
            live_tasks.fetch_add(1, std::memory_order_acq_rel);
        } else {
            std::size_t current = live_tasks.load(std::memory_order_acquire);
            while (current > 0 &&
                   !live_tasks.compare_exchange_weak(current, current - 1,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            }
        }
        // The caller may be the watcher thread, never the worker. The sink
        // never blocks and refuses only a closed broadcaster (termination):
        // that is the one case worth a warning, because "dropped" events
        // during normal operation are the designed degradation for a stalled
        // subscriber -- the transcript, not this status channel, is the
        // authoritative record.
        if (!sink(SessionEvent{task})) {
            // DefaultLogger is process-lifetime; the session logger is the
            // same object. This lambda can outlive `logger_`'s session, but
            // not the logger itself.
            common::DefaultLogger().warn("session {}: background task event for '{}' dropped "
                                         "(session terminated)",
                                         session_id, task.task_id);
        }
    };

    context.tool_lifecycle_sink = [this, sink = event_sink_](const ToolLifecycleEvent& lifecycle) {
        sink(SessionEvent{lifecycle});

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
        observers_.NotifyToolExecuted(InfoSnapshot(), std::move(invocation));
    };

    // Returning false aborts the provider request. This is the cancellation path
    // that termination reuses -- there is no second mechanism, and adding one is
    // how a session ends up with two half-working ways to stop.
    //
    // The sink's IsCancelled check is the cancel half (a full queue can no
    // longer refuse anything -- the sink is drop-oldest, never blocking). The
    // refusal false is now purely Terminate: closed means the session is
    // ending, and the request must stop.
    llm::StreamEventSink stream_event_sink = [this, sink = event_sink_](const llm::StreamEvent& stream) {
        if (IsCancelled()) return false;
        return sink(SessionEvent{stream});
    };

    // The real summarizer. Passed as the plain function rather than wrapped,
    // because everything it needs -- provider, model, prompt, cancel flag -- is
    // an argument the loop supplies per call. That is what keeps a mid-session
    // /agent switch from compacting with the previous preset's prompt.
    //
    // loop_config.EffectiveCompactor() (S4) resolves the configured strategy's
    // summarize half (memory.compaction_strategy, set once in main.cpp) or
    // falls back to compact_messages -- the producer-side default, so this
    // call site does not need to know what the fallback is.
    //
    // seed is non-default only on resume: the stored transcript becomes the
    // loop's working history WITHOUT being re-appended to the store it was
    // loaded from. See ResumeSeed for what its fields carry.
    loop_ = std::make_unique<AgentLoop>(
        *provider_, executor_, registry, *store_, preset_, loop_config, std::move(context),
        [this] { return IsCancelled(); }, std::move(stream_event_sink),
        loop_config.EffectiveCompactor(), std::move(seed), std::move(params_.project_context));

    // Constructor-injected wiring (Phase 5 step 3): formerly set by
    // SessionRegistry AFTER construction, via set_presets/set_commands/
    // set_provider_factories/set_provider_instance_cache -- a window during
    // which the worker thread below already existed but these were still
    // null. See SessionParams for what each is borrowed from.
    presets_ = params_.presets;
    commands_ = params_.commands;
    provider_factories_ = params_.provider_factories;
    provider_instance_cache_ = params_.provider_instance_cache;
    // Phase 5 step 8: the same copy executor_'s init list above already used,
    // kept here for kUserPromptSubmit -- one field, not a second
    // SessionParams::hooks in sync with LoopConfig::hooks by convention.
    hooks_ = loop_config.hooks;

    // MCP liveness at session start, BEFORE the worker exists: this thread is
    // allowed to block (the RPC reader is a different one), and the loop must
    // already carry its final server set when the first turn can run.
    mcp_manager_ = params_.mcp_manager;
    if (mcp_manager_ != nullptr) RefreshMcpServers();

    observers_.NotifySessionCreated(InfoSnapshot());
    worker_ = std::thread([this] { WorkerMain(); });
}

void Session::RefreshMcpServers() {
    // A fresh reappraisal every call, not a sticky per-session blocklist: a
    // server that failed once used to stay disabled for the rest of the
    // session's life, and an /agent switch was the only way back -- it never
    // took the chance. EnsureAlive's own cooldown (tools.mcp.restart_cooldown_ms)
    // is what now keeps a genuinely-still-down server from being re-attempted
    // on every call here, without permanently hiding one that came back.
    std::vector<std::string> live;
    live.reserve(preset_.allowed_mcp_servers.size());
    for (const auto& server : preset_.allowed_mcp_servers) {
        std::string error;
        if (mcp_manager_->EnsureAlive(server, error)) {
            live.push_back(server);
            continue;
        }
        // Surfaced, not swallowed: an error on the outbound stream is the
        // same channel a failed turn uses, so the client sees WHY the
        // server's tools never appeared.
        EmitEvent(SessionEvent{SessionError{error}});
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
        // must learn it was not accepted. The sink never blocks (this runs on
        // the reader, which must never block even on its own error path), and
        // it only refuses a closed broadcaster.
        if (!EmitEvent(SessionEvent{SessionError{"session backlog is full; turn refused"}})) {
            logger_.warn("session {}: turn refused and the queue refused the error too; client will not see it",
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
    // Refusals surface on the notice line like any other error --
    // CommandResult::Error's default display (kStatus) is already what the
    // wire wants here.
    if (!EmitEvent(SessionEvent{SessionCommandResult{command.request_id,
                                                     commands::CommandResult::Error(reason)}})) {
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
    // The rebind takes mutex_ because ProviderSnapshot() hands the shared_ptr to
    // the titler thread, and a concurrent reassign would be a data race; the
    // titler tolerates the momentary null a snapshot between the two moves can
    // see (it degrades to no title).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto previous = std::move(provider_);
        provider_ = std::move(rebuilt);
    }
    loop_->SetProvider(*provider_);
}

void Session::ExecuteCommand(const QueuedCommand& command, bool busy) {
    if (commands_ == nullptr || presets_ == nullptr) {
        // No registry wired: answer rather than drop, or a client waiting on a
        // CommandResult hangs on a request nobody will ever handle.
        EmitCommandResult(
            commands::CommandResult::Error("the /-command subsystem is not available in this session"),
            command);
        return;
    }

    commands::CommandContext context(*this, *presets_, *confirmations_, logger_);
    auto outcome = commands::Dispatch(*commands_, command.name, command.args, context, busy);
    EmitCommandResult(std::move(outcome), command);
}

void Session::EmitCommandResult(commands::CommandResult result, const QueuedCommand& command) {
    // The sink never blocks: ExecuteCommand runs on the worker AND on the RPC
    // layer's command pool. A refusal means the broadcaster is closed
    // (termination); capacity never refuses -- a stalled subscriber drops its
    // own oldest instead.
    if (!EmitEvent(SessionEvent{SessionCommandResult{command.request_id, std::move(result)}})) {
        logger_.warn("session {}: broadcaster closed; CommandResult for '{}' dropped",
                     params_.session_id, command.name);
    }
}

void Session::RunOneCommand(const QueuedCommand& command) {
    // Reached only from the worker, and only between turns -- so `busy` is
    // false by construction here too. The parameter exists for the reader-side
    // refusal path, which checks before queueing.
    ExecuteCommand(command, /*busy=*/false);
}

bool Session::SubmitRewind(QueuedRewind request) {
    // Malformed before queued: neither action named. The queue would only
    // defer the same answer, and nothing about it depends on the worker.
    if (!request.redo && request.target_seq < 0) {
        if (!EmitEvent(SessionEvent{
                SessionError{"rewind request names neither a target seq nor a redo"}})) {
            logger_.warn("session {}: malformed rewind refused and the queue is full",
                         params_.session_id);
        }
        return false;
    }
    // Subagent refusal is reader-side: it is a property of params alone, no
    // worker pass could change it, and a child has no user to edit its prompt.
    if (!params_.parent_session_id.empty()) {
        if (!EmitEvent(SessionEvent{SessionError{"subagent sessions cannot be rewound"}})) {
            logger_.warn("session {}: subagent rewind refused and the queue is full",
                         params_.session_id);
        }
        return false;
    }

    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return false;
        // The SAME queue as turns and commands, deliberately: a rewind runs at
        // a turn boundary, and a turn queued behind it forks the rewound
        // branch -- queue order IS the semantics.
        if (work_.size() >= kMaxWorkItems) {
            refused = true;
        } else {
            work_.push_back(std::move(request));
        }
    }
    if (refused) {
        // Same contract as SubmitTurn: refusal answered on the wire, never
        // dropped. The sink never blocks on the reader thread.
        if (!EmitEvent(SessionEvent{SessionError{"session backlog is full; rewind refused"}})) {
            logger_.warn("session {}: rewind refused and the queue is full; client will not "
                         "see it",
                         params_.session_id);
        }
        return false;
    }
    turn_cv_.notify_one();
    return true;
}

// A rewind target must be a REAL user turn: role User carrying text. This
// excludes injected subagent completions (User role, tool-result blocks only)
// and anything else a human never typed -- rewinding "to" one would cut the
// transcript at a position the user cannot re-enter as a prompt.
bool IsRewindTarget(const session::StoredMessage& stored) {
    if (stored.message.role != llm::Role::User) return false;
    for (const auto& block : stored.message.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
            if (!text->text.empty()) return true;
        }
    }
    return false;
}

void Session::RunOneRewind(const QueuedRewind& request) {
    // The traversal machinery lives behind the SQLite-backed store and reader.
    // A plain test session on a fake store has neither, and refuses with the
    // reason rather than pretending.
    if (!store_->SupportsRewind() || history_reader_ == nullptr) {
        EmitRewindRefusal("rewind requires the SQLite-backed session store");
        return;
    }
    try {
        if (request.redo) {
            RunRedo();
        } else {
            RunRewind(request.target_seq);
        }
    } catch (const std::exception& e) {
        logger_.warn("session {}: rewind failed: {}", params_.session_id, e.what());
        EmitRewindRefusal(e.what());
    }
}

void Session::RunRewind(std::int64_t target_seq) {
    // Validation reads through the reader, which already filters discarded
    // rows: a rewound-away position comes back as absent and is refused --
    // there is no rewinding into a discarded branch.
    const auto target = history_reader_->LoadMessage(params_.session_id, target_seq);
    if (!target || !IsRewindTarget(*target)) {
        EmitRewindRefusal("rewind target " + std::to_string(target_seq) +
                          " is not an active user message");
        return;
    }

    // Store first, then memory. A crash between the two leaves the durable
    // active set ahead of the working set -- the resume path loads the
    // truncated transcript, which is the state the user asked for. The reverse
    // order would leave memory describing turns the store still counts, and
    // the next append would land on top of rows the user rewound away.
    store_->SetDiscardedAfter(target_seq);

    // TWO memory halves, decided by the checkpoint boundary. Above it, the
    // summary still describes only kept turns and the working set is truncated
    // in place. At or below it, the summary over-covers turns that did not
    // happen -- and dropping it wholesale would also lose the still-active
    // turns BELOW the cut that it summarized, so the working set is rebuilt
    // from the store's active prefix and the checkpoint (the cached view being
    // discarded) is cleared with it. Best-effort, like every checkpoint write:
    // a failed clear degrades to a resume restoring a stale summary, never to
    // a lost transcript.
    const auto boundary = loop_->CheckpointBoundary();
    if (boundary.has_value() && target_seq <= *boundary) {
        const auto rows = history_reader_->LoadAll(params_.session_id);
        std::vector<llm::Message> messages;
        messages.reserve(rows.size());
        for (auto& stored : rows) messages.push_back(std::move(stored.message));
        try {
            store_->ClearCompactionCheckpoint();
        } catch (const std::exception& error) {
            logger_.warn("session {}: failed to clear compaction checkpoint: {}",
                         params_.session_id, error.what());
        }
        loop_->ReseedFromStore(std::move(messages),
                               rows.empty() ? 0 : rows.front().seq);
    } else {
        loop_->TruncateAfter(target_seq);
    }

    observers_.NotifySessionRewound(info_, target_seq);
    // The active transcript now ends just before the target; -1 when the
    // target was seq 0.
    EmitSessionRewound(target_seq, target_seq - 1);
}

void Session::RunRedo() {
    // The range to restore is computed BEFORE the unmark: it is the active
    // rows above the current tail, and after the unmark they are active rows
    // too -- indistinguishable from what was never discarded.
    const std::int64_t pre_active_last = history_reader_->ActiveLastSeq(params_.session_id);
    const std::int64_t restored = store_->UnmarkDiscarded();
    if (restored == 0) {
        // Either nothing was ever discarded or the fork already reclaimed it.
        // Both mean the same thing to the user: there is no branch to go back
        // to.
        EmitRewindRefusal("nothing to redo");
        return;
    }

    // The restored rows join the working set WITHOUT being re-appended -- the
    // resume seed's shape, applied to a live loop. LoadAfter reads the
    // just-restored rows; they are active again.
    const auto rows = history_reader_->LoadAfter(params_.session_id, pre_active_last);
    if (!rows.empty()) {
        std::vector<llm::Message> messages;
        messages.reserve(rows.size());
        for (auto& stored : rows) messages.push_back(std::move(stored.message));
        loop_->RestorePersisted(std::move(messages), rows.front().seq);
    }

    observers_.NotifySessionRewound(info_, -1);
    EmitSessionRewound(-1, history_reader_->ActiveLastSeq(params_.session_id));
}

void Session::EmitRewindRefusal(const std::string& reason) {
    // Worker path. The sink never refuses for capacity, so a refusal the
    // subscriber needs can only be lost by termination itself.
    EmitEvent(SessionEvent{SessionError{reason}});
}

void Session::EmitSessionRewound(std::int64_t truncated_to_seq, std::int64_t active_last_seq) {
    EmitEvent(SessionEvent{SessionRewound{truncated_to_seq, active_last_seq}});
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
    // The advisor's abandoned thread: a slow Advise polling past this cancel
    // observes it and returns per its contract, instead of polling a stale
    // "never cancelled" forever.
    if (advisor_cancel_flag_) {
        advisor_cancel_flag_->store(true, std::memory_order_release);
    }
    // Bump the generation: every turn popped BEFORE this point observes a
    // cancelled() that is now true; a turn popped after records the new
    // generation as its base and runs uncancelled. One mechanism, no
    // clear-timing race in either direction.
    cancel_gen_.fetch_add(1, std::memory_order_acq_rel);
    // Fire the turn's token: a tool parked on its cv (MCP call) wakes now,
    // not on its next poll slice. Everything a live token's waiters do from
    // here is the same path a deadline takes -- a cancelled tool call.
    cancel_source_.CancelAll();
    // (No queue unwedge any more: the sink is drop-oldest and never blocks, so
    // no producer can be parked on a full subscriber queue for a cancel to
    // reach. The old AbortWaitingProducers existed for exactly that park.)
    // Wakes a worker parked on a permission prompt. Without this, cancelling
    // during an Ask would set a flag nobody is in a position to read.
    //
    // CancelAll, not Shutdown: this ends the TURN. The distinction is
    // load-bearing -- a permanent cancel here leaves every later permission
    // prompt in the session answering "no answer" instantly, which
    // WirePermissionChecker turns into Deny, so one CancelTurn would silently
    // deny-lock a session that reports itself alive and keeps accepting turns.
    cancel_pending_requests_();
}

void Session::JoinWorkerOnce() {
    std::call_once(join_once_, [this] {
        if (worker_.joinable()) worker_.join();
    });
}

void Session::Terminate() {
    // Same fire the advisor's Cancel gets: an Advise still running at teardown
    // must see the session is going away, and a DETACHED advise thread polling
    // after this function returns must read a live flag, not a destroyed
    // Session.
    if (advisor_cancel_flag_) {
        advisor_cancel_flag_->store(true, std::memory_order_release);
    }
    if (terminated_.exchange(true, std::memory_order_acq_rel)) {
        // Already terminating. Still join, so that a second caller does not
        // return while the worker is alive -- the destructor relies on it.
        if (worker_.get_id() != std::this_thread::get_id()) JoinWorkerOnce();
        return;
    }

    // NOT a cancel-generation bump: Terminate ends the SESSION, and IsCancelled
    // already folds terminated_ in. It DOES fire the cancel tokens, though --
    // a tool blocked on a token cv must wake to unwind. Shutting pending
    // requests is the other half of the same wakeup: it releases a worker
    // parked on a gate mid-teardown -- including one parked in
    // WirePermissionChecker::check on a question the client will never answer.
    cancel_source_.CancelAll();
    shutdown_pending_requests_();
    // Refuses future out-of-band pushes: a background task finishing mid-
    // teardown learns "session gone" from its schedule_injection returning
    // false, exactly like a subagent's completion hook. Already-staged
    // messages stay drainable but nobody will drain them -- the worker is
    // exiting, and a session nobody is attached to loses its in-flight
    // injections by design.
    channels_->injections.Close();
    turn_cv_.notify_all();

    // THE TERMINAL WIRE EVENT, published to every subscriber before closing
    // the transport so each writer drains it. A session terminated under
    // attached streams -- a child whose blocking spawn was cancelled, a
    // reaper eviction -- used to go silent: the attached handler looped on a
    // queue that would never produce again and the client could not
    // distinguish "killed" from "finished quietly". The sink never blocks and
    // cannot refuse for capacity; the only refusal is a broadcaster that is
    // already closed.
    if (!EmitEvent(SessionEvent{SessionEnded{}})) {
        logger_.warn("session {}: SessionEnded dropped (queue full or closed)",
                     params_.session_id);
    }

    // Close the transport AFTER the join above would be too late: every
    // subscriber queue closes drain-first, so each attached writer delivers
    // what it holds (the SessionEnded event among it) and then unwinds, and
    // the worker cannot be parked on a full queue any more (the sink never
    // blocks), so the join below cannot deadlock the way the old
    // blocking-Push design feared.
    close_transport_();

    if (worker_.get_id() != std::this_thread::get_id()) JoinWorkerOnce();

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

    observers_.NotifySessionEnded(InfoSnapshot());
}

SessionInfo Session::InfoSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

void Session::SetName(std::string name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.name = name;
    }
    // The sink never blocks (the titler runs on a detached thread); after
    // Terminate the event is simply lost -- the session is gone, so there is
    // nobody to refresh.
    SessionMetadataChanged metadata;
    metadata.name = std::move(name);
    metadata.partial = true;
    EmitEvent(SessionEvent{std::move(metadata)});
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
            // The advisor flag is TURN-SCOPED like the generation above: a
            // cancel that landed during a previous turn must not make every
            // later Advise abstain forever. Reset at pop -- the same
            // pop-to-clear window the generation mechanism exists to close
            // cannot bite here, because a stale reader is an ALREADY-ABANDONED
            // advise thread whose poll result is discarded either way, and it
            // polls a live shared atomic regardless.
            if (advisor_cancel_flag_) {
                advisor_cancel_flag_->store(false, std::memory_order_release);
            }
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
        } else if (auto* rewind = std::get_if<QueuedRewind>(&item)) {
            RunOneRewind(*rewind);
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

    // --- core.user_prompt_submit: the prompt veto seam -------------------------
    //
    // On the WORKER, deliberately, not in SubmitTurn: SubmitTurn runs on the
    // gRPC reader, which must never block, and a hook action is a subprocess
    // with a budget. The observable semantics are unchanged -- nothing is
    // persisted, and the client gets an error -- but the wedge risk lands
    // on the thread that is allowed to block.
    //
    // TOP-LEVEL ONLY: a subagent's seeded task is the parent model's own
    // formulation, not a user prompt, and the parent already decided to send
    // it. Blocking it here would strand a spawner waiting on a completion the
    // child is forbidden to produce. A seedless wake turn (nullopt) is not a
    // user prompt either -- its content is injected messages the model itself
    // asked for, so it passes unvetted the same way.
    //
    // A veto sets the flag and FALLS THROUGH to the bottom block; it does not
    // return early. The bottom block is the every-exit-path obligation (wire
    // TurnCompleted, observer notification, the subagent completion hook) --
    // an early return skipped all of it once, leaking per-turn observer state
    // on every vetoed prompt. `result` stays default-constructed, so the
    // shared exit reports exactly what the old hand-rolled veto emit did:
    // StopReason::Unknown, empty usage, hit_max_turns false.
    bool vetoed = false;
    if (hooks_ != nullptr && params_.parent_session_id.empty() && text) {
        nlohmann::json payload{{"session_id", params_.session_id},
                               {"project_root", params_.project_root.string()},
                               {"prompt", *text}};
        if (auto blocked = hooks_->Emit(hooks::kUserPromptSubmit, std::move(payload),
                                        [this] { return IsCancelled(); })) {
            logger_.warn("session {}: user_prompt_submit hook blocked the prompt: {}",
                         params_.session_id, *blocked);
            EmitEvent(SessionEvent{SessionError{"prompt rejected by hook: " + *blocked}});
            vetoed = true;
        }
    }

    TurnResult result;
    std::int64_t first_seq = 0;
    if (!vetoed) {
        try {
            first_seq = store_->last_appended_seq() + 1;
            if (text) {
                // @-mention expansion is for the HUMAN's typed turns only: a subagent's
                // task arrives from the model, and expanding '@' in model-generated prose
                // would misfire on emails and ordinary sentences. No mentions -> a message
                // byte-identical to the plain wrap (mention_expansion.hpp), so the
                // unconditional-looking ternary changes nothing for anyone else.
                llm::Message seed = params_.parent_session_id.empty()
                                        ? ExpandMentions(*text, params_.project_root, logger_)
                                        : llm::Message{llm::Role::User, {llm::TextBlock{std::move(*text)}}};
                result = loop_->RunTurn(std::move(seed));
            } else {
                // The wake path: no seed. The turn's user content is whatever the
                // loop flushes at turn start -- the injected messages themselves.
                result = loop_->RunTurn();
            }
        } catch (const llm::LlmError& error) {
            // The loop rethrows once retries are exhausted. The session survives:
            // one failed turn is not a reason to tear down a conversation the user
            // can retry.
            EmitEvent(SessionEvent{error});
            logger_.warn("session {}: turn failed: {}", params_.session_id, error.what());
        } catch (const std::exception& e) {
            EmitEvent(SessionEvent{SessionError{e.what()}});
            logger_.warn("session {}: turn threw: {}", params_.session_id, e.what());
        } catch (...) {
            // Neither an LlmError nor a std::exception. The catches above existed to
            // keep one failed turn from taking down the worker -- and with it the
            // process -- but anything else escaping here would still have reached
            // std::terminate through WorkerMain. Same treatment: one degraded turn,
            // a live session.
            EmitEvent(SessionEvent{SessionError{"the turn threw an unrecognized exception"}});
            logger_.warn("session {}: turn threw a non-standard exception", params_.session_id);
        }
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
    // The name refresh runs BEFORE EmitTurnCompleted, deliberately:
    // TurnCompletedEvent is the END-OF-TURN signal -- "the only thing that
    // tells a client the server is done talking for now" (proto comment) -- and
    // a client may half-close the moment it sees it. Anything pushed after it
    // is then an undrained message on a stream the client has stopped reading,
    // which wedges the client's Finish() (the drain-before-Finish deadlock § A
    // records, from the server side). Bookkeeping events ride BEFORE the
    // terminator, never after.
    try {
        MaybeEmitNameFromStore();
    } catch (...) {
        logger_.warn("session {}: checking the store for a default name failed", params_.session_id);
    }
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
        summary.model = std::move(result.model);
        summary.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        observers_.NotifyTurnCompleted(InfoSnapshot(), std::move(summary));
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
    // This is the turn's exit signal, and a headless client detects the end
    // of a turn by it alone. The sink never blocks and never refuses for
    // capacity -- a full subscriber drops its oldest to make room -- so the
    // terminator always lands on every attached stream unless the
    // broadcaster is already closed.
    EmitEvent(SessionEvent{TurnCompleted{stop_reason, usage, hit_max_turns}});
}

void Session::MaybeEmitNameFromStore() {
    // Two exit conditions before the query: already named (the ordinary case --
    // a chosen name, a resumed session, or this method having run once), or
    // nothing appended yet (a turn that failed before its first message; there
    // is no name to find and the next turn will ask again).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.name.empty()) return;
    }
    if (store_->last_appended_seq() < 0) return;

    const auto stored = store_->StoredName();
    if (!stored || stored->empty()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.name = *stored;
    }
    SessionMetadataChanged metadata;
    metadata.name = *stored;
    metadata.partial = true;
    EmitEvent(SessionEvent{std::move(metadata)});
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
