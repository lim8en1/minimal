#include "minimal/agent/session.hpp"

#include <algorithm>

#include "minimal/mcp/mcp_client_manager.hpp"

#include "minimal/agent/provider_resolver.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "minimal/agent/mention_expansion.hpp"
#include "minimal/agent/session_naming.hpp"
#include "minimal/config/permission_codec.hpp"

namespace minimal::agent {
namespace {

// Fails construction loudly, before any member is built, rather than letting
// a missing wiring field surface later as a silently-dropped event. Every
// real session gets these from the registry's wiring step
// (SessionRegistry::CreateResolved); a test that constructs a Session
// directly must call rpc::FinishSessionWiring (or supply fakes) itself first.
SessionParams&& RequireWiring(SessionParams&& params) {
    if (!params.transport) {
        throw std::invalid_argument("SessionParams::transport is required");
    }
    if (!params.permission_checker) {
        throw std::invalid_argument("SessionParams::permission_checker is required");
    }
    if (!params.confirmation_broker) {
        throw std::invalid_argument("SessionParams::confirmation_broker is required");
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
      ask_broker_(std::move(params_.ask_broker)),
      transport_(std::move(params_.transport)),
      event_sink_([transport = transport_](const SessionEvent& event) {
          return transport->Publish(event);
      }),
      // The run mode the client asked for wins over the configured default: it
      // is a property of the CLIENT, not of the config. A headless run has no
      // human to answer a prompt, so it must be able to say so. apply_run_mode()
      // is applied downstream, once, by ToolExecutor. For a subagent the
      // spawner has already clamped the override against the parent's run mode,
      // and hands down the parent's policies as ceilings and the parent's
      // grants as seed -- the whole inheritance lands in this one call.
      executor_(registry, *checker_, preset_.permission_mode, preset_.mcp_permissions,
                params_.run_mode_override.value_or(preset_.run_mode), params_.policy_ceiling,
                params_.mcp_policy_ceiling, params_.seed_grants, loop_config.hooks),
      // The sticky record starts where the executor starts; every later
      // writer (SetRunMode, the borrow path) keeps the two in sync.
      // ApplyPreset deliberately is NOT a writer: a preset re-resolution is
      // a model/policy swap, not a posture change.
      desired_run_mode_(params_.run_mode_override.value_or(preset_.run_mode)) {
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
    // Same "remember the choice" rule for /subagent-model: the value is
    // consumed by SubagentManager::Spawn, never by this session's own turns.
    subagent_model_override_ = params_.subagent_model_override;

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

    // The interactive ask: the loop copies this into every ToolContext (see
    // SessionContext::ask_user). Null broker leaves it null -- the tool
    // degrades -- rather than wiring a call that would dereference nothing.
    if (ask_broker_) {
        tools::AskBroker& broker = *ask_broker_;
        context.ask_user = [&broker](const tools::AskRequest& request) {
            return broker.Ask(request);
        };
    }

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

    // A compaction rewrites only the working set, never a transcript row, so
    // this event is the client's one live signal that it happened (the same
    // record rides scrollback pages via the compactions table).
    context.compaction_sink = [sink = event_sink_](const CompactionEvent& compaction) {
        sink(SessionEvent{compaction});
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
        auto labels = executor_.hook_labels(lifecycle.tool_name, lifecycle.input);
        invocation.permission_key = std::move(labels.permission_key);
        invocation.category = std::move(labels.category);
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

    // The real-time steering gate. Runs on the WORKER (the loop's flush is
    // worker-only), which is what makes the hook subprocess safe here -- the
    // same thread contract as the turn-pop veto in RunOneTurn. A user-text
    // injection that the core.user_prompt_submit hook blocks is dropped
    // before it is persisted, with the reason on the wire -- identical
    // observable semantics to a vetoed turn-pop. A subagent never steers (its
    // seeded task is not a user prompt; its sessions receive no user turns),
    // mirroring the turn-pop seam's top-level-only rule. hooks_ is read at
    // call time -- it is assigned below, before the worker starts.
    context.steer_gate = [this, sink = event_sink_](const llm::Message& message) {
        if (hooks_ == nullptr || !params_.parent_session_id.empty()) return true;
        std::string prompt;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (!prompt.empty()) prompt += "\n";
                prompt += text->text;
            }
        }
        nlohmann::json payload{{"session_id", params_.session_id},
                               {"project_root", params_.project_root.string()},
                               {"prompt", prompt}};
        if (auto blocked = hooks_->Emit(hooks::kUserPromptSubmit, std::move(payload),
                                        [this] { return IsCancelled(); })) {
            logger_.warn("session {}: user_prompt_submit hook blocked a steered message: {}",
                         params_.session_id, *blocked);
            sink(SessionEvent{SessionError{"steered message rejected by hook: " + *blocked}});
            return false;
        }
        return true;
    };

    // The real summarizer. Resolved by the loop PER COMPACTION ATTEMPT -- see
    // AgentLoop::Compact: a mid-session /agent switch must change the summary
    // format the way it already changes compression_prompt, and a preset can
    // pin the format with `compaction_strategy:` frontmatter (resolved against
    // the registry borrowed into params_.compaction_strategies). What is
    // passed here is the injected compactor (tests) or nullopt-equivalent;
    // everything else the loop resolves live.
    //
    // seed is non-default only on resume: the stored transcript becomes the
    // loop's working history WITHOUT being re-appended to the store it was
    // loaded from. See ResumeSeed for what its fields carry.
    //
    // on_provider_round: the loop's per-round usage signal routed to the
    // observer registry -- the same fire-and-forget dispatch every other
    // notification takes. InfoSnapshot() is read at CALL time (not captured),
    // so a round dispatched mid-turn reports the session's current name, the
    // same freshness OnTurnCompleted's snapshot has.
    loop_config.on_provider_round = [this](agent::RoundUsage round) {
        observers_.NotifyProviderRound(InfoSnapshot(), std::move(round));
    };
    loop_ = std::make_unique<AgentLoop>(
        *provider_, executor_, registry, *store_, preset_, loop_config, std::move(context),
        [this] { return IsCancelled(); }, [this] { return paused(); },
        std::move(stream_event_sink), Compactor{}, std::move(seed),
        std::move(params_.project_context), params_.environment.compaction_strategies);

    // Borrowed process-wide wiring; see SessionParams.
    presets_ = params_.environment.presets;
    commands_ = params_.environment.commands;
    project_commands_ = params_.environment.project_commands;
    provider_resolver_ = params_.environment.provider_resolver;
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
        // A preset naming a server that is configured but currently disabled
        // (SetEnabled / mcps.json) is an intent to use it -- auto-enable
        // rather than silently lose those tools. Failure here (only possible
        // if the name vanished from handles_ between StatusOf and SetEnabled,
        // which cannot happen since nothing removes a handle post-construction)
        // falls through to the ordinary EnsureAlive failure path below.
        const auto status = mcp_manager_->StatusOf(server);
        if (status.configured && !status.enabled) {
            std::string enable_error;
            if (mcp_manager_->SetEnabled(server, true, enable_error)) {
                logger_.info("session {}: auto-enabled MCP server '{}' required by preset '{}'",
                             params_.session_id, server, preset_.name);
            }
        }
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
    QueuedTurn turn;
    turn.text = std::move(text);
    return SubmitTurnWithOverrides(std::move(turn));
}

bool Session::SubmitTurnWithOverrides(std::string text, const std::optional<common::ModelRef>& model,
                                      const std::string& agent_preset) {
    QueuedTurn turn;
    turn.text = std::move(text);
    if (model) turn.model = model->str();
    turn.agent_preset = agent_preset;
    return SubmitTurnWithOverrides(std::move(turn));
}

bool Session::SubmitTurnWithOverrides(QueuedTurn turn) {
    // Set => the session refuses this turn outright (terminated/finished, or
    // the backlog check below); emitted as a SessionError once the lock is
    // released. Every refusal reason must go through this, not a bare
    // `return false` under the lock: SubmitTurn's contract (session.hpp) is
    // that false always comes with a visible error on the wire, so a client
    // is never left waiting on a turn that was silently dropped.
    std::optional<std::string> refusal;
    bool steered = false;
    bool auto_resumed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) {
            refusal = "session already terminated";
        } else if (subagent_finished_.load(std::memory_order_acquire)) {
            // A subagent's life is ONE turn: once it has finished, a further
            // turn would run a session its spawner has already collected and
            // reported on.
            refusal = "subagent session already finished";
        } else {
            // AUTO-RESUME: a user message while paused IS the intent to continue,
            // unless the operator opted out. Cleared before routing so the
            // mid-turn case (steering, below) lets the running turn continue past
            // the pause check and answer the message within itself, and the idle
            // case queues a normal turn. The metadata event -- what every
            // attached status bar refreshes from -- goes out after the lock.
            if (IsPaused(pause_state_.load(std::memory_order_acquire)) &&
                params_.pause_auto_resume_on_submit) {
                ClearPausedLocked();
                auto_resumed = true;
            }
            // REAL-TIME STEERING: a plain text turn arriving while the AGENT is
            // mid-turn (worker_activity_ == kRunningTurn, not just "the worker is
            // busy" -- a command running is not a turn to ride) does not queue
            // behind the turn; it is staged onto the injection queue and
            // appended at the next tool-result boundary, so
            // the very next provider request carries
            // [..., assistant(tool_use), user(tool_results), user(steering)].
            // Overrides opt OUT: a mid-turn model/preset switch is a re-resolution
            // the running loop cannot absorb, so those ride the work queue as
            // today. The steering gate (core.user_prompt_submit) fires at flush
            // time on the worker.
            //
            // OpenToSteering() closes the window between the turn's POP and its
            // seed landing in the history: a submission there is ahead of the
            // seed on the clock but behind it in submission order, and steering
            // it would flush it BEFORE the seed (RunTurn's turn-start flush
            // precedes AppendAndPersist) -- reversing on the durable transcript
            // the order the user actually typed. Such a submission queues on the
            // work queue and runs its own turn. (Out-of-band injections --
            // subagent completions, background results -- keep the turn-start
            // flush's before-the-seed position: they are not user submissions
            // and their chronology genuinely precedes the seed's turn.)
            if (worker_activity_ == WorkerActivity::kRunningTurn && loop_->OpenToSteering() &&
                turn.model.empty() && turn.agent_preset.empty()) {
                // The id is assigned for steered submissions too: TurnQueuedEvent's
                // per-session monotonicity holds, and the id is what the client
                // correlates its optimistic echo against even though there is
                // nothing to cancel.
                turn.turn_id = next_turn_id_++;
                steered = channels_->injections.Push(
                    llm::Message{llm::Role::User, {llm::TextBlock{turn.text}}});
                if (!steered) refusal = "session backlog is full; turn refused";
            } else if (work_.size() >= kMaxWorkItems) {
                refusal = "session backlog is full; turn refused";
            } else {
                turn.turn_id = next_turn_id_++;
                work_.push_back(turn);  // copied: turn_id and text are announced below
            }
        }
    }
    if (auto_resumed) EmitMetadataChanged();
    if (refusal && !steered) {
        // Answer the refusal rather than dropping it: the client sent a turn and
        // must learn it was not accepted. The sink never blocks (this runs on
        // the reader, which must never block even on its own error path), and
        // it only refuses a closed broadcaster.
        if (!EmitEvent(SessionEvent{SessionError{*refusal}})) {
            logger_.warn("session {}: turn refused and the queue refused the error too; client will not see it",
                         params_.session_id);
        }
        return false;
    }
    // Announce the acceptance with its id: a mid-turn submit is QUEUED (it
    // runs only after the current turn ends, and its seed user row persists
    // only at pop), so without this the submitting client has nothing to
    // address a per-message cancel at -- and no confirmation that the
    // message is held at all. A STEERED message carries steered=true: it is
    // not on the work queue, so there is nothing for CancelQueuedTurn to
    // address -- the client renders "delivers at the next tool boundary"
    // instead of a cancel affordance, and the transcript row appears when
    // the boundary flush lands it.
    if (SessionEvent evt{TurnQueued{turn.turn_id, turn.text, steered}}; !EmitEvent(evt)) {
        // Non-fatal: the turn is queued and will run regardless; only the
        // cancel affordance is degraded on this client.
        logger_.warn("session {}: turn_queued event dropped by the queue",
                     params_.session_id);
    }
    turn_cv_.notify_one();
    return true;
}

bool Session::SubmitCommand(QueuedCommand command) {
    // Every refusal branch below must reject through RejectCommand, not a
    // bare `return false`: a client waiting on this request_id's
    // CommandResult must not hang because the command was silently dropped
    // (the terminated_ branch used to do exactly that).
    std::optional<std::string> refusal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) {
            refusal = "session already terminated";
        } else if (work_.size() >= kMaxWorkItems) {
            // The SAME queue as turns, deliberately. Two queues would have no
            // order between them, so `/model x` typed before a turn could
            // apply after it.
            refusal = "session backlog is full";
        } else {
            work_.push_back(std::move(command));
        }
    }
    if (refusal) {
        // Same contract as SubmitTurn, plus the request_id: a client waiting on
        // a CommandResult must not hang because its command was not accepted.
        RejectCommand(QueuedCommand{std::move(command)}, *refusal);
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
    // it: /agent to a preset on another backend or entry, or /model across
    // providers, both land here.
    const bool provider_changed = !ProviderResolver::SameConnection(preset, preset_);

    // THE PROVIDER IS ACQUIRED FIRST, before anything below commits: preset_,
    // loop_'s preset and EmitMetadataChanged all describe the NEW preset, so a
    // failed acquisition must return having changed nothing. The previous
    // instance needs no teardown -- the resolver's cache still holds it for
    // any other session using it.
    std::shared_ptr<llm::LlmProvider> rebuilt;
    if (provider_changed) {
        if (provider_resolver_ == nullptr) {
            logger_.warn("session {}: cannot switch provider; keeping the previous preset",
                         params_.session_id);
            return;
        }
        try {
            rebuilt = provider_resolver_->Acquire(preset);
        } catch (const std::exception& error) {
            logger_.warn("session {}: {}; keeping the previous preset", params_.session_id,
                         error.what());
            return;
        }
    }

    // Worker thread, between turns: the loop is provably not inside a provider
    // call, so the swap is safe. It takes mutex_ because preset_snapshot() and
    // ProviderSnapshot() read these off the worker (command pool, titler).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preset_ = std::move(preset);
        if (rebuilt) provider_ = std::move(rebuilt);
        // The preset NAME is external-callers' data (SessionInfo): the
        // metadata event and the tab label read it from info_.
        if (!preset_.name.empty()) info_.agent_preset = preset_.name;
    }
    // The run mode is left alone: it is the user's durable choice, not the
    // preset's (no preset declares one; the registry stamps the global default
    // on every resolve), and SetRunMode may be writing it from the reader right
    // now. The policies DO follow the preset, or /agent would change the
    // prompt and tools while the old preset's gates stayed in force.
    ApplyEffective(preset_, *provider_, std::nullopt);
    // The new preset may allow different MCP servers. Runs on the worker at a
    // turn boundary (the same blocking contract as a preset swap): a dead
    // server in the new list is surfaced here, never discovered mid-turn.
    if (mcp_manager_ != nullptr) RefreshMcpServers();

    // Announce the full metadata set so every attached client's status bar
    // ({model} {agent} {permission_mode} {tokens}-max) refreshes NOW instead
    // of only at its next attach.
    EmitMetadataChanged();
}

void Session::ApplyEffective(const AgentPreset& preset, llm::LlmProvider& provider,
                             std::optional<tools::PermissionRunMode> run_mode) {
    loop_->SetPreset(preset);
    loop_->SetProvider(provider);
    executor_.SetPolicies(preset.permission_mode, preset.mcp_permissions);
    if (run_mode) executor_.SetRunMode(*run_mode);
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

    // Resolved fresh per command rather than cached on the session: For()'s
    // own cache makes a second call for the same root free, and a session's
    // project_root never changes after creation anyway -- there is nothing
    // to invalidate.
    // Held for the whole dispatch: a rebuild (an edited command file) may
    // replace the root's registry while this command still executes.
    const std::shared_ptr<const commands::CommandRegistry> project_holder =
        project_commands_ != nullptr ? project_commands_->For(info_.project_root) : nullptr;
    const commands::CommandRegistry* project_registry = project_holder.get();
    // SUBAGENT GUARD, defense in depth: the reader (SessionServiceImpl::
    // DispatchCommand) already refuses non-read-only commands for a child, but
    // this is the one place every dispatch path converges, so the invariant
    // "a subagent never executes a mutating command" holds even if a future
    // call site bypasses the reader. Answered, never dropped: a client waiting
    // on a CommandResult would otherwise hang.
    if (!params_.parent_session_id.empty()) {
        const commands::Command* resolved =
            commands::FindEffective(*commands_, project_registry, command.name);
        if (resolved != nullptr && !resolved->ReadOnly()) {
            EmitCommandResult(commands::CommandResult::Error(
                                  commands::SubagentRefusalMessage(command.name)),
                              command);
            return;
        }
    }
    commands::CommandContext context(this, *presets_, *confirmations_, logger_, mcp_manager_,
                                     project_registry);
    auto outcome =
        commands::Dispatch(*commands_, command.name, command.args, context, busy, project_registry);
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

    std::optional<std::string> refusal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) {
            refusal = "session already terminated";
        } else if (work_.size() >= kMaxWorkItems) {
            // The SAME queue as turns and commands, deliberately: a rewind runs
            // at a turn boundary, and a turn queued behind it forks the
            // rewound branch -- queue order IS the semantics.
            refusal = "session backlog is full; rewind refused";
        } else {
            work_.push_back(std::move(request));
        }
    }
    if (refusal) {
        // Same contract as SubmitTurn: refusal answered on the wire, never
        // dropped. The sink never blocks on the reader thread.
        if (!EmitEvent(SessionEvent{SessionError{*refusal}})) {
            logger_.warn("session {}: rewind refused and the queue is full; client will not "
                         "see it",
                         params_.session_id);
        }
        return false;
    }
    turn_cv_.notify_one();
    return true;
}

bool Session::SetRunMode(tools::PermissionRunMode mode) {
    // THE READER-THREAD control event, like SetPaused: the mode must be able
    // to land MID-TURN (a shift+tab pressed while the agent sits on a gated
    // tool call is exactly the operator's "let it through"), and queueing it
    // for the worker would apply it only at the turn boundary -- precisely
    // the delay the switch exists to avoid. Safe because the executor's run
    // mode is a single atomic enum: every read (ResolveMode's apply_run_mode,
    // a spawner's clamp, metadata emission) is a lock-free load, so the
    // cross-thread store adds no race the reads did not already cover. This
    // used to be a queued work item on the theory that SetRunMode was
    // worker-thread-only; the queue bought a turn-boundary delay and no
    // safety.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return false;
        desired_run_mode_ = mode;
    }
    // The executor's own atomic; the sticky record is already updated. A
    // permission Ask already on the wire is NOT retroactively answered by
    // this -- the operator still answers that prompt, and every LATER gate
    // (the same turn's next tool call included) sees the new mode.
    executor_.SetRunMode(mode);
    // What every attached client's {permission_mode} refreshes from. The sink
    // never blocks -- the same contract SetPaused's reader-side emission
    // already relies on.
    EmitMetadataChanged();
    return true;
}

tools::PermissionRunMode Session::desired_run_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return desired_run_mode_;
}

// A rewind target must be a REAL user turn: role User carrying text. This
// excludes injected subagent completions (User role, tool-result blocks only)
// and anything else a human never typed -- rewinding "to" one would cut the
// transcript at a position the user cannot re-enter as a prompt.
bool IsRewindTarget(const session::StoredMessage& stored) {
    return IsUserTextMessage(stored.message);
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

void Session::EmitMetadataChanged() {
    const auto preset = preset_snapshot();
    // InfoSnapshot(), not info_ directly: SetName() (the titler's observer
    // thread) writes info_.name under mutex_, and this runs on the worker --
    // a bare read here races that write (TSan-visible; a torn std::string
    // read is a real crash, not just a stale value).
    const auto info = InfoSnapshot();
    SessionMetadataChanged metadata;
    metadata.name = info.name;
    metadata.agent_preset = info.agent_preset;
    metadata.model = preset.model_ref.str();
    metadata.permission_mode = config::ToString(executor_.run_mode());
    metadata.context_window_tokens = preset.context_window_tokens.value_or(0);
    metadata.paused = paused();
    if (!EmitEvent(SessionEvent{std::move(metadata)})) {
        logger_.warn("session {}: metadata event dropped (broadcaster closed)",
                     params_.session_id);
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

    observers_.NotifySessionRewound(InfoSnapshot(), target_seq);
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

    observers_.NotifySessionRewound(InfoSnapshot(), -1);
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
    transport_->CancelPending();
    // CANCEL BEATS PAUSE. Cancel is "stop now" -- the operator escalating past
    // a pause they just set (or forgot about). Leaving the paused component
    // set would fence the very next turn behind a state the operator just
    // overrode. LOCKED, unlike the old independent-atomic paused_.store: once
    // pause and continuation share one atomic, "clear only the paused half"
    // is a read-modify-write, not a context-free store, and Cancel() (a
    // reader-thread method, like SetPaused) holds no other lock here so this
    // adds no deadlock risk.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearPausedLocked();
    }
}

void Session::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return;
        PauseState cur = pause_state_.load(std::memory_order_relaxed);
        if (IsPaused(cur) == paused) return;
        const bool had_continuation = HasContinuationPending(cur);
        pause_state_.store(paused
                                ? (had_continuation ? PauseState::kPausedWithContinuation
                                                    : PauseState::kPaused)
                                : (had_continuation ? PauseState::kContinuationPending
                                                    : PauseState::kNotPaused),
                           std::memory_order_release);
        // EXPLICIT RESUME KICK. Only when the unpause has something to
        // continue into: the last turn actually ended paused
        // (a continuation is pending) -- not one that ended EndTurn on its own
        // while the flag was set -- and nothing else is queued or running. A
        // queued turn (auto-resume ran) or the worker still finishing the
        // final round makes the kick redundant; a turn that continues past an
        // unpauses' mid-flight clears the flag at its own end. The kick is
        // the seedless wake turn: the model continues from the persisted tool
        // results, answering any steered message that flushed after them.
        if (!paused && worker_activity_ == WorkerActivity::kIdle &&
            HasContinuationPending(pause_state_.load(std::memory_order_relaxed)) &&
            work_.empty() && !subagent_finished_.load(std::memory_order_acquire)) {
            if (work_.size() < kMaxWorkItems) {
                work_.push_back(QueuedResume{});
                ClearContinuationLocked();
            }
            // Over the bound: drop the kick silently -- the session was
            // idle (the checks above), so the bound cannot be hit here;
            // the guard is structural, not operational.
        }
    }
    EmitMetadataChanged();
    // The worker may be parked on a predicate this change flips in either
    // direction (hold a turn it was about to pop; release held turns; run the
    // resume kick). The empty critical section above closed the lost-wakeup
    // window -- the same shape QueueInjectedMessage and Terminate guard.
    turn_cv_.notify_all();
}

void Session::CancelQueuedTurn(std::uint64_t turn_id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = work_.begin(); it != work_.end(); ++it) {
            auto* turn = std::get_if<QueuedTurn>(&*it);
            if (turn == nullptr || turn->turn_id != turn_id) continue;
            work_.erase(it);
            removed = true;
            break;
        }
    }
    // Always answered: the caller must learn removed=false (already popped --
    // it must run, and the pending echo stays) just as surely as removed=true.
    if (!EmitEvent(SessionEvent{TurnCanceled{turn_id, removed}})) {
        logger_.warn("session {}: turn_canceled event dropped by the queue",
                     params_.session_id);
    }
}

void Session::CancelAllQueuedTurns() {
    std::vector<std::uint64_t> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = work_.begin(); it != work_.end();) {
            if (auto* turn = std::get_if<QueuedTurn>(&*it)) {
                dropped.push_back(turn->turn_id);
                it = work_.erase(it);
            } else {
                // Commands, rewinds: the flush is about PENDING MESSAGES, and
                // the current turn is untouched either way. A queued /model
                // the user asked for one submission ago is not silently undone
                // by cancelling the queue's messages.
                ++it;
            }
        }
    }
    for (const auto turn_id : dropped) {
        if (!EmitEvent(SessionEvent{TurnCanceled{turn_id, true}})) {
            logger_.warn("session {}: turn_canceled event dropped by the queue",
                         params_.session_id);
        }
    }
}

void Session::JoinWorkerOnce() {
    std::call_once(join_once_, [this] {
        if (worker_.joinable()) worker_.join();
    });
}

void Session::FireOnTurnFinishedOnce(const TurnResult& result, std::string_view context) {
    if (!params_.on_turn_finished) return;
    if (subagent_finished_.exchange(true, std::memory_order_acq_rel)) return;
    try {
        params_.on_turn_finished(result);
    } catch (...) {
        logger_.warn("session {}: on_turn_finished threw{}", params_.session_id, context);
    }
}

void Session::ClearPausedLocked() {
    const PauseState cur = pause_state_.load(std::memory_order_relaxed);
    if (IsPaused(cur)) {
        pause_state_.store(HasContinuationPending(cur) ? PauseState::kContinuationPending
                                                        : PauseState::kNotPaused,
                           std::memory_order_release);
    }
}

void Session::ClearContinuationLocked() {
    const PauseState cur = pause_state_.load(std::memory_order_relaxed);
    if (HasContinuationPending(cur)) {
        pause_state_.store(IsPaused(cur) ? PauseState::kPaused : PauseState::kNotPaused,
                           std::memory_order_release);
    }
}

void Session::MarkContinuationPendingLocked() {
    const PauseState cur = pause_state_.load(std::memory_order_relaxed);
    pause_state_.store(IsPaused(cur) ? PauseState::kPausedWithContinuation
                                     : PauseState::kContinuationPending,
                       std::memory_order_release);
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
    transport_->ShutdownPending();
    // Refuses future out-of-band pushes: a background task finishing mid-
    // teardown learns "session gone" from its schedule_injection returning
    // false, exactly like a subagent's completion hook. Already-staged
    // messages stay drainable but nobody will drain them -- the worker is
    // exiting, and a session nobody is attached to loses its in-flight
    // injections by design.
    channels_->injections.Close();
    // Empty locked section before the notify: the worker's wait predicate
    // reads terminated_ under mutex_ (WorkerMain), and every write to the
    // predicate above this point (terminated_.exchange, the two lines above)
    // happened without that lock. Without this, the worker can evaluate the
    // predicate (seeing terminated_ == false) strictly between the exchange
    // and this notify, park on the condition variable having already missed
    // it, and never wake -- the same lost-wakeup shape QueueInjectedMessage
    // already guards against with an identical empty critical section.
    { std::lock_guard<std::mutex> lock(mutex_); }
    turn_cv_.notify_all();

    // THE TERMINAL WIRE EVENT, published to every subscriber before closing
    // the transport so each writer drains it. A session terminated under
    // attached streams -- a child whose blocking spawn was cancelled, a
    // reaper eviction -- used to go silent: the attached handler looped on a
    // queue that would never produce again and the client could not
    // distinguish "killed" from "finished quietly". The sink never blocks and
    // cannot refuse for capacity; the only refusal is a broadcaster that is
    // already closed.
    //
    // GUARDED, individually, the same way RunOneTurn guards each of its
    // post-turn stages separately rather than under one enclosing try: an
    // exception leaving Terminate() is UB or worse everywhere it is called
    // from (~Session() is implicitly noexcept, the reaper thread and
    // TerminateAll() have no handler either -- an uncaught throw here is
    // std::terminate on an ordinary session-lifetime path, not an exotic
    // one). A single try wrapping everything below would trade that hazard
    // for a different correctness bug: JoinWorkerOnce() below MUST run even
    // if EmitEvent or transport_->Close() throws, or the worker thread is never
    // joined and this object is unsafe to destroy. Guarding each step lets an
    // earlier failure skip only its own work, never the later steps'.
    try {
        if (!EmitEvent(SessionEvent{SessionEnded{}})) {
            logger_.warn("session {}: SessionEnded dropped (queue full or closed)",
                         params_.session_id);
        }
    } catch (...) {
        logger_.warn("session {}: emitting SessionEnded threw during teardown",
                     params_.session_id);
    }

    // Close the transport AFTER the join above would be too late: every
    // subscriber queue closes drain-first, so each attached writer delivers
    // what it holds (the SessionEnded event among it) and then unwinds, and
    // the worker cannot be parked on a full queue any more (the sink never
    // blocks), so the join below cannot deadlock the way the old
    // blocking-Push design feared.
    try {
        transport_->Close();
    } catch (...) {
        logger_.warn("session {}: closing the transport threw during teardown", params_.session_id);
    }

    if (worker_.get_id() != std::this_thread::get_id()) JoinWorkerOnce();

    // The subagent completion hook fires on EVERY exit path -- including a
    // terminate that lands before the seeded turn ever ran (the initial-attach
    // reaper, or a terminate racing SubmitTurn). Without this, a blocking parent
    // waits on a promise that nothing will ever set.
    TurnResult cancelled;
    cancelled.stop_reason = llm::StopReason::Cancelled;
    FireOnTurnFinishedOnce(cancelled, " during teardown");

    try {
        observers_.NotifySessionEnded(InfoSnapshot());
    } catch (...) {
        logger_.warn("session {}: notifying session-ended observers threw during teardown",
                     params_.session_id);
    }
}

SessionInfo Session::InfoSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

namespace {

std::optional<std::string> RefString(const std::optional<common::ModelRef>& ref) {
    return ref ? std::optional<std::string>(ref->str()) : std::nullopt;
}

}  // namespace

// The in-memory override is what this session runs on; the store's record is
// what a later resume reads. Persisting is best-effort: a failed write only
// costs a resume the choice.
void Session::set_model_override(std::optional<common::ModelRef> ref) {
    model_override_ = std::move(ref);
    try {
        store_->PersistModelOverride(RefString(model_override_));
    } catch (const std::exception& e) {
        logger_.warn("session {}: persisting model override failed: {}", params_.session_id,
                     e.what());
    }
}

void Session::set_subagent_model_override(std::optional<common::ModelRef> ref) {
    subagent_model_override_ = std::move(ref);
    try {
        store_->PersistSubagentModel(RefString(subagent_model_override_));
    } catch (const std::exception& e) {
        logger_.warn("session {}: persisting subagent-model override failed: {}",
                     params_.session_id, e.what());
    }
}

bool Session::RenameSession(const std::string& name) {
    if (name.empty()) return false;
    try {
        if (!store_->Rename(name)) return false;
    } catch (const std::exception& e) {
        logger_.warn("session {}: renaming failed: {}", params_.session_id, e.what());
        return false;
    }
    SetName(name);
    return true;
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
    // Partial emissions carry the pause state too: a bool has no "absent"
    // wire encoding, so a client applying metadata unconditionally would
    // read a name-only event's default (false) as "unpaused" and clear its
    // indicator on every rename. Carrying the live value makes every
    // metadata event authoritative for paused.
    metadata.paused = paused();
    EmitEvent(SessionEvent{std::move(metadata)});
}

void Session::WorkerMain() {
    while (true) {
        WorkItem item;
        bool wake_turn = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            turn_cv_.wait(lock, [this] {
                if (terminated_.load(std::memory_order_acquire)) return true;
                if (!work_.empty()) {
                    // PAUSE HOLD: while paused, only non-turn work may run --
                    // commands, rewinds, the resume kick. This is the "pause
                    // to reconfigure" affordance: /agent, /model and a rewind
                    // all execute against a paused session, while its held
                    // turns wait for unpause (auto-resume via a new
                    // submission, or SetPaused(false)). Turns are LEFT IN
                    // PLACE, not reordered around. (A run-mode change is not
                    // here by design: it applies on the reader, immediately --
                    // see SetRunMode.)
                    if (!IsPaused(pause_state_.load(std::memory_order_acquire))) return true;
                    for (const auto& candidate : work_) {
                        if (!std::holds_alternative<QueuedTurn>(candidate)) {
                            return true;
                        }
                    }
                    return false;
                }
                // Idle: the wake paths. All of them hold while paused --
                // a paused session makes no provider call nobody asked for.
                if (IsPaused(pause_state_.load(std::memory_order_acquire))) return false;
                if (ShouldWakeForInjections()) return true;
                // Real-time steering sweep: a steering message that missed
                // its turn's flush (the model had already returned EndTurn)
                // is a human prompt sitting unanswered. It wakes the worker
                // for a seedless continuation turn REGARDLESS of
                // wake_on_injection -- a user message is explicit intent,
                // which a background result is not. Keyed on the HISTORY
                // (the durable record), not the injection queue: the turn's
                // final flush may already have drained the queue and
                // appended the message unanswered.
                if (channels_->injections.HasUserText() ||
                    (HistoryEndsWithUnansweredUserText() && last_turn_natural_)) {
                    return true;
                }
                // The explicit-resume kick raced a still-running final round:
                // SetPaused(false) found the worker still busy and skipped
                // enqueueing; the turn then ended Paused. The continuation
                // survives (kContinuationPending), and this predicate is what
                // turns it into the actual continuation.
                return HasContinuationPending(pause_state_.load(std::memory_order_acquire));
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
                if (!IsPaused(pause_state_.load(std::memory_order_acquire))) {
                    item = std::move(work_.front());
                    work_.pop_front();
                } else {
                    // The predicate guaranteed a non-turn item exists; find
                    // it and leave any held turns in place.
                    for (auto it = work_.begin(); it != work_.end(); ++it) {
                        if (std::holds_alternative<QueuedTurn>(*it)) {
                            continue;
                        }
                        item = std::move(*it);
                        work_.erase(it);
                        break;
                    }
                }
                // A real turn (seeded or the resume kick) starting means the
                // "last turn ended paused" record is consumed: leaving it set
                // would let a LATER unpause spuriously re-run a continuation
                // for a session that has moved on.
                if (std::holds_alternative<QueuedTurn>(item) ||
                    std::holds_alternative<QueuedResume>(item)) {
                    ClearContinuationLocked();
                }
            } else {
                // Only reachable when a wake condition fired: an out-of-band
                // message (ShouldWakeForInjections, with the wake setting on)
                // -- or a steering message that missed its flush, or a resume
                // kick whose enqueue lost its race -- needs a SEEDLESS turn
                // whose user content is the injected messages / the persisted
                // history itself.
                wake_turn = true;
                ClearContinuationLocked();
            }
            // kRunningOther for now; RunOneTurn itself upgrades to kRunningTurn
            // for turn-shaped items (the QueuedTurn/QueuedResume branches
            // below) once it actually starts. Commands and rewinds stay
            // kRunningOther for their whole span.
            worker_activity_ = WorkerActivity::kRunningOther;
        }

        // The turn-boundary guarantee, and it is structural rather than checked:
        // this pops one item at a time and RunOneTurn is atomic from here, so a
        // command can only ever land BETWEEN turns -- never between a tool_use
        // and its tool_result.
        //
        // A subagent's life is ONE turn, and SubmitTurn's check alone does not
        // close it: an item queued while the seeded turn is still running is
        // popped here only AFTER that turn ended (setting subagent_finished_).
        // Re-check at pop time and drop EVERY item kind -- a command or rewind
        // would act on a session its spawner has already collected just as
        // wrongly as a turn would. The seeded turn itself is safe: the flag is
        // set only when it ENDS, and it was popped while the flag was still
        // false.
        if (!subagent_finished_.load(std::memory_order_acquire)) {
            // Catch-all, not just std::exception: a plugin command throwing a
            // non-standard exception out of RunOneCommand must not terminate
            // the worker thread -- every other queue drain in the process
            // contains, and this one is the heart of the session.
            try {
                if (wake_turn) {
                    RunOneTurn(std::nullopt);
                } else if (auto* turn = std::get_if<QueuedTurn>(&item)) {
                    RunOneTurn(std::move(turn->text), turn);
                } else if (std::holds_alternative<QueuedResume>(item)) {
                    // The explicit-resume kick (SetPausedEvent{false}): the
                    // seedless wake turn, continuing from the persisted tool
                    // results the paused turn left behind.
                    RunOneTurn(std::nullopt);
                } else if (auto* rewind = std::get_if<QueuedRewind>(&item)) {
                    RunOneRewind(*rewind);
                } else {
                    RunOneCommand(std::get<QueuedCommand>(item));
                }
            } catch (const std::exception& e) {
                logger_.error("session {}: worker item threw: {}", params_.session_id, e.what());
            } catch (...) {
                logger_.error("session {}: worker item threw a non-standard exception",
                              params_.session_id);
            }
        }
        // else: dropped silently -- the session is finished and collected;
        // there is no client stream left worth answering on.

        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_activity_ = WorkerActivity::kIdle;
        }
        turn_cv_.notify_all();
    }
}

bool Session::ShouldWakeForInjections() const {
    // Called from the wait predicate, under mutex_. Lock order is one-way:
    // mutex_ -> the queue's own mutex; producers (subagent completion hooks,
    // background watchers) hold only the queue's mutex, so no cycle exists.
    if (!params_.wake_on_injection) return false;
    if (worker_activity_ != WorkerActivity::kIdle || !work_.empty()) return false;
    // A finished subagent's life is over regardless of what is staged.
    if (subagent_finished_.load(std::memory_order_acquire)) return false;
    return !channels_->injections.empty();
}

bool Session::HistoryEndsWithUnansweredUserText() const {
    // Worker-only: the working history mutates on the worker, and both
    // callers (the wait predicate, the wake branch) run there.
    return loop_->HistoryEndsWithUnansweredUserText();
}

void Session::RunOneTurn(std::optional<std::string> text, const QueuedTurn* overrides) {
    const auto started = std::chrono::steady_clock::now();

    // THE AGENT IS NOW MID-TURN. worker_activity_ == kRunningTurn is what
    // SubmitTurn's real-time steering routing keys on, and it must hold for
    // exactly the span of the turn -- not of a command (kRunningOther serves
    // that coarser role). RAII, not a post-call clear: an exception out of
    // RunTurn's catchable surface is contained below, but the worker's own
    // catch-all is the backstop, and a stuck kRunningTurn would silently
    // divert every future submission onto the steering path. Drops back to
    // kRunningOther, not kIdle -- WorkerMain still owns the transition to
    // kIdle once this whole work item (not just the in-turn span) is done.
    struct InTurnGuard {
        Session* session;
        ~InTurnGuard() {
            std::lock_guard<std::mutex> lock(session->mutex_);
            session->worker_activity_ = WorkerActivity::kRunningOther;
        }
    } in_turn_guard{this};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_activity_ = WorkerActivity::kRunningTurn;
    }

    // Computed from the RAW text, before @-mention expansion below may move
    // out of `text` -- a tab label reflects what the user typed, not inlined
    // @file contents. Empty for a seedless wake turn (no candidate) and
    // harmless to compute even for a vetoed or failed turn: the post-turn
    // write is guarded by "still unnamed" either way. The EMIT below is
    // veto-gated though: a prompt the user_prompt_submit hook rejected must
    // never name the session, even one that is still unnamed with prior
    // content -- the rejected text was never persisted.
    const std::string default_name_candidate = text ? DefaultSessionName(*text) : std::string();

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

    // --- per-turn model/agent overrides ---------------------------------------
    //
    // Applied on the WORKER at turn pop -- the same boundary invariant that
    // makes /model and /agent safe (the loop is provably not inside a
    // provider call). The sticky state (preset_, model_override_, the
    // metadata stream) is untouched: the turn borrows, the session keeps.
    //
    // Precedence: the turn's explicit model wins over the session's pinned
    // model_override_, which wins over the (possibly overridden) preset's own
    // model. The borrowed preset's run mode can only NARROW the sticky one
    // (clamp_run_mode) -- a per-turn switch is a borrow, never an escalation.
    // Restore is a scope guard so EVERY exit path -- LlmError, cancellation,
    // a non-standard exception out of RunTurn -- puts back exactly what the
    // session holds; a leaked borrow would silently run every later turn
    // on the wrong model. (A local class inside a member function sees the
    // member's access.)
    struct PresetRestore {
        Session* session = nullptr;
        bool active = false;
        // The restore target is desired_run_mode_, read AT RESTORE TIME, not
        // a snapshot of the executor taken at pop. A snapshot races the
        // reader-side switch in both directions: the queued shift+tab of the
        // original bug wrote the executor behind the snapshot's back and the
        // restore silently reverted the user's mode -- and a reader-side
        // SetRunMode landing DURING a borrowed turn (the mode can now change
        // mid-turn) would be reverted by a pop-time snapshot the same way.
        // The user's latest word outranks the borrow.
        ~PresetRestore() {
            if (!active || session == nullptr) return;
            // Unconditional, provider included, so a borrow can never leak
            // into the next turn.
            session->ApplyEffective(session->preset_, *session->provider_,
                                    session->desired_run_mode());
        }
    } preset_restore;
    preset_restore.session = this;

    // Holds the per-turn borrowed provider for the rest of this function:
    // loop_->SetProvider stores a reference, which must stay valid through
    // RunTurn and past preset_restore's rebind back to provider_.
    std::shared_ptr<llm::LlmProvider> borrowed_provider;

    if (overrides && (!overrides->model.empty() || !overrides->agent_preset.empty())) {
        try {
            // active before ANY mutation, and kept set through the catch
            // below: a borrow that throws mid-way (after SetRunMode/
            // SetPreset, during provider construction) must still unwind to
            // the pre-borrow state, not leave a half-applied borrow behind.
            // On a fully-failed borrow the restore is a harmless no-op rebind.
            preset_restore.active = true;
            AgentPreset transient = preset_;
            if (!overrides->agent_preset.empty()) {
                if (presets_ == nullptr) {
                    throw std::runtime_error("no preset registry wired; cannot resolve agent '" +
                                             overrides->agent_preset + "'");
                }
                transient = presets_->Resolve(overrides->agent_preset, params_.project_root);
            }
            // Model precedence: the turn's explicit ref wins over the session's
            // pinned model_override_, which wins over the preset's own model.
            // The ref is split here (pop time), so a malformed one fails the
            // turn by name instead of being dropped reader-side.
            std::optional<common::ModelRef> model;
            if (!overrides->model.empty()) {
                model = common::SplitModelRef(overrides->model);
                if (!model) {
                    throw std::runtime_error("malformed model '" + overrides->model +
                                             "'; expected \"<provider>/<model>\"");
                }
            } else {
                model = model_override_;
            }
            if (model) {
                if (provider_resolver_ == nullptr) {
                    throw std::runtime_error("no provider resolver wired; cannot resolve model '" +
                                             model->str() + "'");
                }
                transient = provider_resolver_->ResolveModel(std::move(transient), model);
            }
            // A different instance only when the borrowed preset is served by
            // a different connection; the cache makes a known entry a hit.
            if (!ProviderResolver::SameConnection(transient, preset_)) {
                if (provider_resolver_ == nullptr) {
                    throw std::runtime_error("no provider resolver wired; cannot switch provider "
                                             "for the per-turn override");
                }
                borrowed_provider = provider_resolver_->Acquire(transient);
            }
            // The borrowed preset's gates govern the borrowed turn. Its run
            // mode can only narrow the session's CURRENT posture
            // (desired_run_mode_, which every writer keeps in sync), never the
            // default from params_/preset_.
            ApplyEffective(transient, borrowed_provider ? *borrowed_provider : *provider_,
                           tools::clamp_run_mode(desired_run_mode(), transient.run_mode));
        } catch (const std::exception& error) {
            // A resolution failure fails the TURN with a named error -- nothing
            // persisted, the session keeps its sticky choices -- the same
            // fall-through shape the prompt veto uses below. active stays set:
            // the destructor unwinds whatever the borrow already mutated back
            // to the snapshot.
            logger_.warn("session {}: per-turn override failed: {}", params_.session_id,
                         error.what());
            EmitEvent(
                SessionEvent{SessionError{std::string("per-turn override failed: ") + error.what()}});
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
    // THE STEERING SWEEP'S GATE, recorded where `result` is final. True only
    // for a NATURAL model stop -- the model received the conversation and
    // finished replying, so user text appearing AFTER it (the final flush's
    // steering delivery) is a fresh human prompt the model owes a reply to.
    // Every other ending suppresses the sweep: a failed turn's own seed is
    // unanswered user text at the history's end, and so is a turn cancelled
    // before its first response -- answering either automatically would fire
    // a provider call against a dead backend or resurrect a turn the user
    // just halted (and, on a permanently failing provider, spin forever).
    // Vetoed turns fall out with Unknown -- a refused prompt must not be
    // answered by the back door either.
    last_turn_natural_ = result.stop_reason == llm::StopReason::EndTurn ||
                         result.stop_reason == llm::StopReason::MaxTokens ||
                         result.stop_reason == llm::StopReason::StopSequence ||
                         result.stop_reason == llm::StopReason::ContentFilter ||
                         result.stop_reason == llm::StopReason::Refusal;
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
        if (!vetoed) EmitDefaultNameIfComputed(default_name_candidate);
    } catch (...) {
        logger_.warn("session {}: setting the default name failed", params_.session_id);
    }
    try {
        EmitTurnCompleted(result.stop_reason, result.last_usage, result.hit_max_turns,
                          result.usage_estimated);
    } catch (...) {
        logger_.warn("session {}: emitting TurnCompletedEvent failed", params_.session_id);
    }
    try {
        TurnSummary summary;
        summary.first_seq = first_seq;
        summary.last_seq = store_->last_appended_seq();
        if (vetoed) {
            // Nothing was appended, but first_seq kept its 0 initializer while
            // last_seq tracks the real transcript end -- reporting [0, last]
            // claims a turn over rows it never wrote. Report the empty shape a
            // failed turn reports: first past last.
            summary.first_seq = summary.last_seq + 1;
        }
        summary.stop_reason = result.stop_reason;
        summary.usage = result.last_usage;
        summary.usage_estimated = result.usage_estimated;
        // The turn's whole billing shape, not the last round's occupancy
        // snapshot: observers that price usage need the ACROSS-rounds sum and
        // the fully-reported flag to apply the unpriced rule honestly.
        summary.turn_usage = result.turn_usage;
        summary.turn_usage_fully_reported = result.turn_usage_fully_reported;
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
    // Last, after every wire event and observer notification: a blocking
    // parent waiting on this result may proceed the instant the callback
    // returns, and must not observe a session that still has bookkeeping in
    // flight. Fired on EVERY exit path -- `result` is default-constructed
    // after a failed turn, which is exactly what a waiter needs to unblock
    // with an error.
    // EXCHANGE, not store. Terminate() guards the same hook with an exchange
    // (see FireOnTurnFinishedOnce) -- exactly-once holds whatever the
    // ordering between the two, not because either one joins the other's
    // thread first.
    FireOnTurnFinishedOnce(result, "");
    // Record that an explicit unpause has something to continue into. Marked
    // unconditionally on a Paused result -- including when the pause was
    // already cleared (an unpause racing the final round: SetPaused(false)
    // found the worker still busy and skipped enqueueing the kick), because
    // the worker's wait predicate is what turns this into the seedless
    // continuation in exactly that race (kContinuationPending, the
    // not-paused-but-owed case). Staleness is handled at pop: any real turn
    // or kick clears it, so a later unpause can never re-run a continuation
    // for a session that has moved on.
    if (result.stop_reason == llm::StopReason::Paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        MarkContinuationPendingLocked();
    }
}

void Session::EmitTurnCompleted(llm::StopReason stop_reason, const llm::Usage& usage,
                                bool hit_max_turns, bool usage_estimated) {
    // This is the turn's exit signal, and a headless client detects the end
    // of a turn by it alone. The sink never blocks and never refuses for
    // capacity -- a full subscriber drops its oldest to make room -- so the
    // terminator always lands on every attached stream unless the
    // broadcaster is already closed.
    EmitEvent(SessionEvent{TurnCompleted{stop_reason, usage, hit_max_turns, usage_estimated}});
}

void Session::EmitDefaultNameIfComputed(const std::string& candidate) {
    if (candidate.empty()) return;
    // Two exit conditions before the write: already named (the ordinary case --
    // a chosen name, a resumed session, or this method having run once), or
    // nothing appended yet (a turn that failed before its first message; there
    // is no name to set and the next turn will try again).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.name.empty()) return;
    }
    if (store_->last_appended_seq() < 0) return;

    if (!store_->SetDefaultNameIfUnset(candidate)) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.name = candidate;
    }
    SessionMetadataChanged metadata;
    metadata.name = candidate;
    metadata.partial = true;
    metadata.paused = paused();  // see SetName -- every emission is authoritative
    EmitEvent(SessionEvent{std::move(metadata)});
}

bool Session::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return work_.empty() && worker_activity_ == WorkerActivity::kIdle;
}

bool Session::WaitForIdleForTesting(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return turn_cv_.wait_for(lock, timeout, [this] {
        return (work_.empty() && worker_activity_ == WorkerActivity::kIdle) ||
               terminated_.load(std::memory_order_acquire);
    });
}

}  // namespace minimal::agent
