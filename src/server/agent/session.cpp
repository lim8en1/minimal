#include "minimal/agent/session.hpp"

#include <algorithm>


#include "minimal/agent/provider_resolver.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include "minimal/agent/mention_expansion.hpp"
#include "minimal/agent/session_naming.hpp"
#include "minimal/agent/session_sinks.hpp"
#include "minimal/agent/tool_source_activation.hpp"
#include "minimal/agent/turn_borrow.hpp"
#include "minimal/agent/transcript_rewinder.hpp"
#include "minimal/config/permission_codec.hpp"
#include "minimal/common/utf8.hpp"

namespace minimal::agent {
namespace {

// Overall budget for activating a preset's tool-source members (MCP connect +
// tools/list), shared by all members.
constexpr std::chrono::seconds kToolSourceStartupBudget{5};

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
      prompt_broker_(std::move(params_.prompt_broker)),
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
      executor_(registry, *checker_, preset_.permission_mode, PermissionModelsFor(preset_),
                params_.run_mode_override.value_or(preset_.run_mode), params_.policy_ceiling,
                params_.permission_models_ceiling, params_.seed_grants, loop_config.hooks),
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

    SessionSinkDeps sink_deps{logger_,
                              params_.session_id,
                              params_.parent_session_id,
                              params_.project_root,
                              channels_,
                              event_sink_,
                              observers_,
                              loop_config.hooks,
                              history_window_.get(),
                              prompt_broker_.get(),
                              [this] { return IsCancelled(); },
                              [this] { WakeWorker(); },
                              [this] { return InfoSnapshot(); },
                              [this](const std::string& tool_name, const nlohmann::json& input) {
                                  return executor_.hook_labels(tool_name, input);
                              }};
    SessionSinks sinks = BuildSessionSinks(sink_deps);
    SessionContext& context = sinks.context;
    llm::StreamEventSink& stream_event_sink = sinks.stream_event_sink;
    channels_->SetWake(sink_deps.wake_worker);

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

    observers_.NotifySessionCreated(InfoSnapshot());
    worker_ = std::thread([this] { WorkerMain(); });
}

void Session::RefreshToolSources() {
    // See ActivateToolSources for the parallel-with-deadline contract.
    auto activation = ActivateToolSources(preset_.tool_sources, params_.environment.tool_sources,
                                          kToolSourceStartupBudget);
    for (const auto& failure : activation.failures) {
        // Surfaced, not swallowed: an error on the outbound stream is the
        // same channel a failed turn uses, so the client sees WHY the
        // member's tools never appeared.
        EmitEvent(SessionEvent{SessionError{failure.message}});
        logger_.warn("session {}: tool source '{}' member '{}' unavailable: {}",
                     params_.session_id, failure.source_name, failure.member, failure.message);
    }
    loop_->SetToolGroups(std::move(activation.groups));
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
            if (pause_.paused() && params_.pause_auto_resume_on_submit) {
                pause_.ClearPaused();
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
            } else if (work_.full()) {
                refusal = "session backlog is full; turn refused";
            } else {
                turn.turn_id = next_turn_id_++;
                work_.TryPush(turn);  // copied: turn_id and text are announced below
            }
        }
    }
    if (auto_resumed) EmitMetadataChanged();
    if (refusal && !steered) {
        // Answer the refusal rather than dropping it: the client sent a turn and
        // must learn it was not accepted. The sink never blocks (this runs on
        // the reader, which must never block even on its own error path), and
        // it only refuses a closed broadcaster.
        EmitOrWarn(SessionEvent{SessionError{*refusal}}, "turn refusal");
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
        } else if (work_.full()) {
            // The SAME queue as turns, deliberately (see WorkQueue).
            refusal = "session backlog is full";
        } else {
            work_.TryPush(std::move(command));
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

void Session::EmitOrWarn(const SessionEvent& event, std::string_view what) {
    if (!EmitEvent(event)) {
        logger_.warn("session {}: {} dropped (queue full or closed); the client will not see it",
                     params_.session_id, what);
    }
}

void Session::RejectCommand(const QueuedCommand& command, const std::string& reason) {
    // Refusals surface on the notice line like any other error --
    // CommandResult::Error's default display (kStatus) is already what the
    // wire wants here.
    EmitOrWarn(SessionEvent{SessionCommandResult{command.request_id,
                                                 commands::CommandResult::Error(reason)}},
               "command refusal");
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
    // The new preset may allow different tool-source members. Runs on the
    // worker at a turn boundary (the same blocking contract as a preset
    // swap): a dead member in the new list is surfaced here, never
    // discovered mid-turn.
    RefreshToolSources();

    // Announce the full metadata set so every attached client's status bar
    // ({model} {agent} {permission_mode} {tokens}-max) refreshes NOW instead
    // of only at its next attach.
    EmitMetadataChanged();
}

void Session::ApplyEffective(const AgentPreset& preset, llm::LlmProvider& provider,
                             std::optional<tools::PermissionRunMode> run_mode) {
    loop_->SetPreset(preset);
    loop_->SetProvider(provider);
    executor_.SetPolicies(preset.permission_mode, PermissionModelsFor(preset));
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
    commands::CommandContext context(this, *presets_, *confirmations_, logger_,
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
    EmitOrWarn(SessionEvent{SessionCommandResult{command.request_id, std::move(result)}},
               "command result");
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
        EmitOrWarn(SessionEvent{SessionError{"rewind request names neither a target seq nor a redo"}},
                   "malformed rewind refusal");
        return false;
    }
    // Subagent refusal is reader-side: it is a property of params alone, no
    // worker pass could change it, and a child has no user to edit its prompt.
    if (!params_.parent_session_id.empty()) {
        EmitOrWarn(SessionEvent{SessionError{"subagent sessions cannot be rewound"}},
                   "subagent rewind refusal");
        return false;
    }

    std::optional<std::string> refusal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) {
            refusal = "session already terminated";
        } else if (work_.full()) {
            // The SAME queue as turns and commands, deliberately: a rewind runs
            // at a turn boundary, and a turn queued behind it forks the
            // rewound branch -- queue order IS the semantics.
            refusal = "session backlog is full; rewind refused";
        } else {
            work_.TryPush(std::move(request));
        }
    }
    if (refusal) {
        // Same contract as SubmitTurn: refusal answered on the wire, never
        // dropped. The sink never blocks on the reader thread.
        EmitOrWarn(SessionEvent{SessionError{*refusal}}, "rewind refusal");
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

void Session::RunOneRewind(const QueuedRewind& request) {
    // The traversal machinery lives behind the SQLite-backed store and reader.
    // A plain test session on a fake store has neither, and refuses with the
    // reason rather than pretending.
    if (!store_->SupportsRewind() || history_reader_ == nullptr) {
        EmitRewindRefusal("rewind requires the SQLite-backed session store");
        return;
    }
    try {
        TranscriptRewinder rewinder(params_.session_id, *store_, *history_reader_, *loop_, logger_);
        const RewindOutcome outcome =
            request.redo ? rewinder.Redo() : rewinder.Rewind(request.target_seq);
        if (outcome.refusal) {
            EmitRewindRefusal(*outcome.refusal);
            return;
        }
        observers_.NotifySessionRewound(InfoSnapshot(), outcome.truncated_to_seq);
        EmitSessionRewound(outcome.truncated_to_seq, outcome.active_last_seq);
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
    EmitOrWarn(SessionEvent{std::move(metadata)}, "metadata event");
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
    // pending message (WorkerShouldProceed) and runs a seedless turn when
    // params_.wake_on_injection says so. But a predicate change WAKES NOBODY
    // by itself -- the parked worker must be notified, or it sleeps with its
    // predicate already true until some unrelated event lands. The empty
    // critical section closes the lost-wakeup window: a worker either saw
    // (work empty, injections pending) before parking, or was not yet parked
    // and cannot miss the notify because it must acquire mutex_ to park.
    const bool staged = channels_->injections.Push(std::move(message));
    if (staged) {
        WakeWorker();
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
    bool was_paused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_paused = pause_.paused();
        pause_.ClearPaused();
    }
    if (was_paused) {
        // Clients show "paused" from the metadata event, and the worker may be
        // parked on a predicate that held queued turns behind the pause.
        EmitMetadataChanged();
        turn_cv_.notify_all();
    }
}

void Session::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return;
        if (!pause_.SetPaused(paused)) return;
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
            pause_.continuation_pending() && work_.empty() &&
            !subagent_finished_.load(std::memory_order_acquire)) {
            if (work_.TryPush(QueuedResume{})) pause_.ClearContinuation();
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
        removed = work_.RemoveTurn(turn_id);
    }
    // Always answered: the caller must learn removed=false (already popped --
    // it must run, and the pending echo stays) just as surely as removed=true.
    EmitOrWarn(SessionEvent{TurnCanceled{turn_id, removed}}, "turn_canceled event");
}

void Session::CancelAllQueuedTurns() {
    std::vector<std::uint64_t> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped = work_.RemoveAllTurns();
    }
    for (const auto turn_id : dropped) {
        EmitOrWarn(SessionEvent{TurnCanceled{turn_id, true}}, "turn_canceled event");
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
    channels_->ClearWake();  // before this object goes away: see SessionChannels::Wake
    // Empty locked section before the notify: the worker's wait predicate
    // reads terminated_ under mutex_ (WorkerMain), and every write to the
    // predicate above this point (terminated_.exchange, the two lines above)
    // happened without that lock. Without this, the worker can evaluate the
    // predicate (seeing terminated_ == false) strictly between the exchange
    // and this notify, park on the condition variable having already missed
    // it, and never wake -- the same lost-wakeup shape QueueInjectedMessage
    // already guards against with an identical empty critical section.
    WakeWorker();

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
        EmitOrWarn(SessionEvent{SessionEnded{}}, "SessionEnded");
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
    // Tool-source activation (MCP connect + tools/list) runs HERE, before the
    // first queued item, not in the constructor: Create/Resume RPCs have a
    // short client deadline and an unreachable server must never fail them.
    // Work submitted meanwhile simply waits in the queue, so the loop still
    // carries its final group set before the first provider request.
    RefreshToolSources();
    {
        // Startup is over: the loop now carries its final tool-group set.
        // WaitForIdleForTesting reads this so "idle" cannot mean "the worker has
        // not started yet" -- a test reading loop state off-thread would race the
        // startup's writes to it.
        std::lock_guard<std::mutex> lock(mutex_);
        worker_started_ = true;
    }
    turn_cv_.notify_all();
    while (true) {
        WorkItem item;
        bool wake_turn = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            turn_cv_.wait(lock, [this] { return WorkerShouldProceed(WakeSnapshot()); });
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
                // The predicate guaranteed a runnable item exists. While
                // paused that is the first non-turn item; held turns stay in
                // place.
                auto popped = work_.PopRunnable(pause_.paused());
                if (!popped) continue;
                item = std::move(*popped);
                // A real turn (seeded or the resume kick) starting means the
                // "last turn ended paused" record is consumed: leaving it set
                // would let a LATER unpause spuriously re-run a continuation
                // for a session that has moved on.
                if (std::holds_alternative<QueuedTurn>(item) ||
                    std::holds_alternative<QueuedResume>(item)) {
                    pause_.ClearContinuation();
                }
            } else {
                // Only reachable when a wake condition fired: an out-of-band
                // message (WorkerShouldProceed, with the wake setting on)
                // -- or a steering message that missed its flush, or a resume
                // kick whose enqueue lost its race -- needs a SEEDLESS turn
                // whose user content is the injected messages / the persisted
                // history itself.
                wake_turn = true;
                pause_.ClearContinuation();
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

WakeInputs Session::WakeSnapshot() const {
    // Called from the wait predicate, under mutex_. Lock order is one-way:
    // mutex_ -> the queue's own mutex; producers (subagent completion hooks,
    // background watchers) hold only the queue's mutex, so no cycle exists.
    WakeInputs in;
    in.terminated = terminated_.load(std::memory_order_acquire);
    in.paused = pause_.paused();
    in.queue_empty = work_.empty();
    in.queue_has_runnable = work_.HasRunnable(in.paused);
    in.subagent_finished = subagent_finished_.load(std::memory_order_acquire);
    in.wake_on_injection = params_.wake_on_injection;
    in.worker_idle = worker_activity_ == WorkerActivity::kIdle;
    in.injections_pending = !channels_->injections.empty();
    in.injections_have_user_text = channels_->injections.HasUserText();
    in.unanswered_after_natural_turn = last_turn_natural_ && HistoryEndsWithUnansweredUserText();
    in.continuation_pending = pause_.continuation_pending();
    return in;
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
    // The turn borrows; the session keeps (see TurnBorrow). Restored by the
    // destructor on EVERY exit path.
    TurnBorrow borrow(TurnBorrowDeps{
        preset_, provider_, presets_, provider_resolver_, params_.project_root, model_override_,
        [this] { return desired_run_mode(); },
        [this](const AgentPreset& preset, llm::LlmProvider& provider,
               std::optional<tools::PermissionRunMode> run_mode) {
            ApplyEffective(preset, provider, run_mode);
        }});
    if (overrides != nullptr) {
        // A resolution failure fails the TURN with a named error -- nothing
        // persisted, the session keeps its sticky choices -- the same
        // fall-through shape the prompt veto uses.
        if (auto error = borrow.Begin(overrides)) {
            logger_.warn("session {}: per-turn override failed: {}", params_.session_id, *error);
            EmitEvent(SessionEvent{SessionError{"per-turn override failed: " + *error}});
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
        pause_.MarkContinuationPending();
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
        return (worker_started_ && work_.empty() && worker_activity_ == WorkerActivity::kIdle) ||
               terminated_.load(std::memory_order_acquire);
    });
}

}  // namespace minimal::agent
