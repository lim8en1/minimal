#include "minimal/agent/session_registry.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include "minimal/agent/signal_router.hpp"
#include "minimal/agent/transcript_repair.hpp"
#include "minimal/config/project_context.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/session/sqlite_history_store.hpp"

namespace minimal::agent {

SessionRegistry::SessionRegistry(const AgentPresetRegistry& presets, tools::ToolRegistry& tools,
                                 const llm::ProviderFactoryRegistry& providers,
                                 SessionObserverRegistry& observers, session::SessionDatabase& db,
                                 LoopConfig loop_config, common::Logger& logger,
                                 std::chrono::milliseconds initial_attach_guard)
    : presets_(presets),
      tools_(tools),
      providers_(providers),
      provider_cache_(providers, logger),
      observers_(observers),
      db_(db),
      history_reader_(std::make_shared<session::SessionReader>(db_)),
      loop_config_(loop_config),
      logger_(logger),
      initial_attach_guard_(initial_attach_guard) {
    reaper_ = std::thread([this] { ReaperMain(); });
}

SessionRegistry::~SessionRegistry() {
    StopReaper();
    TerminateAll();
}

// Idempotent by the joinable() check: a second call (or the destructor after
// an explicit stop) finds the thread already joined and does nothing.
void SessionRegistry::StopReaper() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    reaper_cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();
}

std::shared_ptr<Session> SessionRegistry::Create(SessionParams params) {
    // Resolve() throws for an unknown preset, and for a preset naming a provider
    // that providers.json does not define. Both propagate: they are the caller's
    // to report, and a session with no provider could not run a single turn.
    //
    // Resolved BEFORE the move: argument evaluation order would otherwise let
    // the lookup read a moved-from name. A copy, not the cached reference: a
    // --model override mutates it, and the registry's cache must keep describing
    // the preset as written.
    //
    // The session's own model_override is passed straight into Resolve() so an
    // unresolvable settings.default_model cannot throw ahead of it: the user
    // named a model, and that has to win even when the preset/settings fallback
    // is broken. CreateResolved's own ResolveModel (below) re-applies the same
    // override afterward -- see its comment -- for the preset-resolves-fine,
    // override-still-wins case; passing it here as well is what rescues the
    // preset-does-NOT-resolve case, which ResolveModel runs too late to fix.
    AgentPreset preset =
        presets_.Resolve(params.agent_preset, params.project_root, std::nullopt,
                         params.model_override);
    return CreateResolved(std::move(params), std::move(preset));
}

std::shared_ptr<Session> SessionRegistry::ForkSession(
    const std::string& source_session_id, std::int64_t target_seq, const std::string& name,
    const std::optional<config::ModelRef>& model, const std::optional<std::string>& agent_preset,
    std::optional<tools::PermissionRunMode> run_mode) {
    // The store mints the child row and copies the transcript prefix in one
    // transaction -- the same "the DATABASE mints the id" ordering every other
    // creation path follows. The fork transaction copies the source's
    // persisted model/subagent_model columns verbatim, and the returned row
    // reports them.
    session::SessionSummary row = db_.ForkSession(source_session_id, target_seq, name,
                                                  agent_preset);

    // The fork's model: an explicit request override wins; otherwise the
    // source's own choice (a /model the user pinned) rides the row -- so a
    // fork of a /model session starts on THAT model, not the preset's
    // default. SplitModelRef re-validates whatever the row holds; an
    // unparseable value is dropped (the fork runs its preset's default)
    // rather than failing a fork whose transcript is perfectly good.
    std::optional<config::ModelRef> fork_model;
    if (model) {
        fork_model = model;
    } else if (row.model) {
        fork_model = config::SplitModelRef(*row.model);
    }

    // The source's sticky subagent-model choice (/subagent-model) travels the
    // same way; a fork's children fall back to the inherit-parent default
    // when the source never named one.
    std::optional<config::ModelRef> fork_subagent_model;
    if (row.subagent_model) {
        fork_subagent_model = config::SplitModelRef(*row.subagent_model);
    }

    SessionParams params;
    params.session_id = row.session_id;
    params.name = name;  // empty rides through: default naming from the first user text
    params.agent_preset = row.agent_preset;  // the row already carries the effective one
    params.project_root = row.project_root;
    params.run_mode_override = run_mode;
    params.model_override = fork_model;
    params.subagent_model_override = fork_subagent_model;
    // The fork's store already holds the copied prefix; params.resume routes
    // creation through LoadResumeSeed exactly like a reopened session --
    // [summary + tail] from a copied checkpoint, occupancy reseeding, and the
    // pre-first-call compact all come for free.
    params.resume = true;

    try {
        return Create(std::move(params));
    } catch (...) {
        // The fork transaction committed, but the session could not be built
        // (unresolvable preset/model). A failed fork leaves nothing behind --
        // the same rollback obligation SubagentManager::Spawn carries.
        try {
            db_.DeleteSession(row.session_id);
        } catch (...) {
            // DeleteSession throwing here would mask the real failure; the
            // orphan row is inert (no messages beyond what the next
            // DeleteSession of the source would cascade... it has none -- it
            // is simply a stub row) and the original error is rethrown below.
        }
        throw;
    }
}

std::shared_ptr<Session> SessionRegistry::CreateResolved(SessionParams params,
                                                           AgentPreset preset,
                                                           std::optional<int> max_turns_override) {
    // THE RESERVATION, before anything else. The session_id is claimed under
    // the registry mutex (against live sessions AND other in-flight
    // constructions), and released either when the built session is inserted
    // or by the guard below when construction throws. Every path into this
    // funnel -- fresh creation, resume, subagent spawn -- pays the same
    // ordering, so a concurrent duplicate of any of them is refused before
    // the transcript load, the torn-tail repair append, or the worker start.
    const std::string session_id = params.session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.count(session_id) || reserved_.count(session_id)) {
            throw SessionAlreadyOpen("session '" + session_id + "' is already open");
        }
        reserved_.insert(session_id);
    }
    struct ReleaseReservation {
        SessionRegistry& registry;
        const std::string& id;
        bool committed = false;
        ~ReleaseReservation() {
            if (committed) return;
            std::lock_guard<std::mutex> lock(registry.mutex_);
            registry.reserved_.erase(id);
        }
    } guard{*this, session_id};

    // Read before params is moved from.
    const bool has_parent = !params.parent_session_id.empty();
    SignalRouter* const signal_router = params.signal_router;
    const std::vector<std::string> signal_selectors = std::move(params.signal_selectors);
    const bool resume = params.resume;

    ApplyProjectContext(params);
    // S5.3: same funnel, same reasoning -- a producer that built its own
    // permission_checker (an advised session) owns its own timeout policy, so
    // this only takes effect on the default WirePermissionChecker/
    // WireConfirmationBroker Session builds when params.permission_checker is
    // null. Overwriting an explicit non-zero value a producer already set
    // would defeat that override, so only the funnel default applies.
    if (params.unattended_prompt_timeout == std::chrono::milliseconds::zero()) {
        params.unattended_prompt_timeout = prompt_timeout_;
    }
    // Builds whatever the advisor-composition callers (ApplyAdvisor,
    // SubagentManager) did not already build for this session: the transport
    // (if no advisor applied), the default WirePermissionChecker (if no
    // advisor applied), and -- always -- the WireConfirmationBroker. Must run
    // AFTER unattended_prompt_timeout resolves just above: both of those
    // defaults are built against it, and this is the one funnel every
    // session (top-level, resume, subagent) passes through after that point.
    rpc::FinishSessionWiring(params);
    AppendSignalContext(params);

    // The user's model outranks the preset's. Applied here rather than inside
    // the preset registry because it is a property of this SESSION, not of the
    // preset -- two sessions on one preset can be running different models. In
    // CreateResolved rather than Create so a SUBAGENT spawned with an inherited
    // preset honours it too.
    preset = ResolveModel(std::move(preset), params.model_override);

    LoopConfig loop_config = loop_config_;
    if (max_turns_override.has_value()) loop_config.max_turns = *max_turns_override;
    // Track O: the same funnel property wires the hook dispatcher. A producer
    // that set one explicitly (a test with its own) wins; everyone else gets
    // the process-wide one without knowing it exists. One field now, not two
    // (Phase 5 step 8): Session reads this LoopConfig, not a second copy on
    // SessionParams.
    if (loop_config.hooks == nullptr) loop_config.hooks = hooks_;

    auto provider = ResolveProvider(preset, params.agent_preset);

    // Track M (docs/EXTENSIBILITY.md §2.1): the read side of the transcript.
    // Set here because this is the one funnel every session passes through --
    // top-level and SubagentManager spawns alike -- so a child's window is
    // bound to the child's own id with no extra code, and its tools see its
    // transcript only. Sessions built outside the registry (plain tests on
    // fake stores) leave it null, and ctx.history is null with it. The SAME
    // registry-lifetime reader every session shares -- see history_reader_.
    params.history_reader = history_reader_;

    // Durable half of the model override (see SessionParams::model_override):
    // every funnel path gets the hook, so a runtime /model switch lands in the
    // row and a later resume re-resolves with the user's choice instead of the
    // preset default. The registry outlives every session, so capturing `this`
    // and the (already read) id by value is safe.
    params.model_override_persist =
        [this, session_id](const std::optional<config::ModelRef>& ref) {
            try {
                db_.UpdateSessionModel(session_id,
                                       ref ? std::optional<std::string>(ref->str())
                                           : std::nullopt);
            } catch (const std::exception& e) {
                // Best effort: the running session keeps its override; the
                // cost of a failed write is only a resume losing the choice.
                logger_.warn("session {}: persisting model override failed: {}", session_id,
                             e.what());
            }
        };
    // Durable half of the subagent-model override (see
    // SessionParams::subagent_model_override): the same funnel, the same
    // best-effort reasoning as the hook above.
    params.subagent_model_persist =
        [this, session_id](const std::optional<config::ModelRef>& ref) {
            try {
                db_.UpdateSessionSubagentModel(session_id,
                                               ref ? std::optional<std::string>(ref->str())
                                                   : std::nullopt);
            } catch (const std::exception& e) {
                logger_.warn("session {}: persisting subagent-model override failed: {}",
                             session_id, e.what());
            }
        };

    // Constructor-injected (Phase 5 step 3): presets/commands/provider
    // factories/instance cache reach Session's ctor on `params`, closing the
    // window a post-construction set_* call used to leave open between the
    // worker thread starting and these becoming non-null.
    ApplyConstructorWiring(params, has_parent);

    ResumeSeed seed = resume ? LoadResumeSeed(session_id) : ResumeSeed{};

    auto store = std::make_unique<session::SqliteHistoryStore>(db_, session_id);
    // loop_config, not loop_config_: CreateResolved may have overridden max_turns
    // for a subagent, whose whole life is one bounded turn.
    auto session = std::make_shared<Session>(std::move(params), std::move(preset),
                                             std::move(provider), std::move(store), tools_,
                                             loop_config, observers_, logger_, std::move(seed));
    // Made findable last: the session is fully wired (constructor-injected)
    // and constructed before PublishSession's locked block registers signal
    // subscriptions and inserts it into sessions_ -- an internal signal or
    // command addressed to this session_id must never be able to observe it
    // before that.
    PublishSession(session_id, session, signal_router, signal_selectors, resume);

    guard.committed = true;
    reaper_cv_.notify_all();
    return session;
}

void SessionRegistry::ApplyProjectContext(SessionParams& params) const {
    // PROJECT CONTEXT (AGENTS.md/CLAUDE.md), loaded HERE -- in the funnel every
    // session passes through: top-level creation, resume, and SubagentManager
    // spawns alike. The file is a property of the session's project_root, which
    // no preset knows; the rendered section rides SessionParams into the loop,
    // which appends it after the preset body on every request. Absence is the
    // normal state and silent; a file that exists but cannot be read is worth
    // one warning, since the operator wrote instructions and got silence.
    std::string error;
    if (auto context = config::LoadProjectContext(params.project_root, project_context_, &error)) {
        params.project_context = config::RenderProjectContextSection(*context);
        logger_.info("session {}: loaded project context from {}", params.session_id,
                     context->source_path.string());
    } else if (!error.empty()) {
        logger_.warn("session {}: {}", params.session_id, error);
    }
}

void SessionRegistry::AppendSignalContext(SessionParams& params) const {
    // Signal-channel instructions ride AFTER the project context in the same
    // system section: both are operator-authored framing of the environment,
    // and both are constant within the session (subscriptions are fixed at
    // start). Appending here, in the funnel, keeps AgentLoop unaware of
    // signals -- it renders one context string.
    if (params.signal_context.empty()) return;
    if (params.project_context.empty()) {
        params.project_context = std::move(params.signal_context);
    } else {
        params.project_context += "\n\n" + params.signal_context;
    }
    params.signal_context.clear();
}

AgentPreset SessionRegistry::ResolveModel(
    AgentPreset preset, const std::optional<config::ModelRef>& model_override) const {
    if (!model_override) return preset;
    const auto model = presets_.providers().Resolve(*model_override);
    if (model == nullptr) {
        throw std::runtime_error("unknown model '" + model_override->str() +
                                 "'; providers.json defines " +
                                 config::DescribeModels(presets_.providers()) +
                                 config::DescribeDiscovery(presets_.providers(), model_override->provider));
    }
    ApplyModel(preset, *model);
    return preset;
}

std::shared_ptr<llm::LlmProvider> SessionRegistry::ResolveProvider(
    const AgentPreset& preset, const std::string& agent_preset_name) {
    // The kind string is what selects a factory. A preset can only reach this
    // point with a provider entry that parsed, so an unregistered kind means a
    // plugin is missing or disabled -- worth naming explicitly, because the
    // symptom otherwise is a session that fails on its first turn for no stated
    // reason.
    if (!providers_.Has(preset.provider_kind)) {
        throw std::runtime_error("agent preset '" + agent_preset_name + "' needs provider kind '" +
                                 preset.provider_kind + "', which no loaded plugin registered");
    }

    // SHARED, not per-session: providers are stateless by contract, so two
    // sessions on one providers.json entry share one instance instead of each
    // paying construction. The entry NAME is the key (immutable at runtime);
    // the model override already applied above does not change it -- two
    // models on one entry share the connection by design.
    auto provider = provider_cache_.GetOrCreate(preset.model_ref.provider, preset.provider_kind,
                                                preset.provider);
    if (provider == nullptr) {
        throw std::runtime_error("provider kind '" + preset.provider_kind +
                                 "' failed to construct");
    }
    return provider;
}

ResumeSeed SessionRegistry::LoadResumeSeed(const std::string& session_id) {
    // RESUME: load the stored transcript as the loop's working history. Loaded,
    // not replayed -- everything below appends NEW messages at MAX(seq) + 1, so
    // the stored rows must not go through the store again or the transcript
    // would duplicate itself.
    //
    // Two load paths. A persisted compaction CHECKPOINT (the cached view the
    // live session left behind) means the working set is [summary + tail]:
    // reconstructing it costs no provider call and stays bounded by the KEPT
    // context rather than the transcript, which is the difference between
    // resuming a session that compacted four times over 3M tokens and trying
    // to summarize half of a 3M-token history through one model window. No
    // checkpoint (never compacted, or a pre-checkpoint session) falls back to
    // the full-transcript load.
    ResumeSeed seed;
    const auto checkpoint = db_.LoadCompactionCheckpoint(session_id);
    if (checkpoint) {
        seed.history.push_back(checkpoint->summary);
        for (auto& stored : history_reader_->LoadAfter(session_id, checkpoint->boundary_seq)) {
            if (!seed.first_persisted_seq) seed.first_persisted_seq = stored.seq;
            seed.history.push_back(std::move(stored.message));
        }
        seed.head_is_summary = true;
        if (!seed.first_persisted_seq) {
            // Empty tail: the boundary is still exactly where the persisted
            // portion will resume on the next append.
            seed.first_persisted_seq = checkpoint->boundary_seq + 1;
        }
        logger_.info("session {}: resuming from compaction checkpoint (covers seq <= {})",
                     session_id, checkpoint->boundary_seq);
    } else {
        for (auto& stored : history_reader_->LoadAll(session_id)) {
            if (!seed.first_persisted_seq) seed.first_persisted_seq = stored.seq;
            seed.history.push_back(std::move(stored.message));
        }
    }

    // A session cancelled mid-tool-execution -- or a process killed between
    // the assistant append and the tool-results append -- ends with an
    // unanswered tool_use, which no provider accepts. Synthesize the
    // missing error results as one REAL message (persisted like any other
    // append) rather than refusing to resume: cancellation is ordinary,
    // and the transcript stays an exact, valid prefix either way. The
    // repair appends at MAX(seq)+1, so a checkpoint boundary (which only
    // covers earlier seqs) is unaffected by it.
    if (auto repair = synthesize_missing_tool_results(seed.history)) {
        logger_.info(
            "session {}: stored transcript ends mid tool call; appending synthesized "
            "error results before resuming",
            session_id);
        db_.AppendMessage(session_id, *repair, 0);
        seed.history.push_back(std::move(*repair));
    }
    return seed;
}

void SessionRegistry::ApplyConstructorWiring(SessionParams& params, bool has_parent) {
    params.presets = &presets_;
    // A SUBAGENT GETS NO COMMAND SURFACE. Commands are a user control channel,
    // and a child has no user: its client attaches to observe, and the spawn was
    // driven by the parent's model rather than by a person with a prompt.
    //
    // It is also an escape hatch if left open. /agent re-resolves a preset from
    // disk, and a freshly resolved preset has is_subagent = false and its own
    // full allowed_tools -- so a child running `/agent build` would get
    // spawn_subagent back and reopen the recursion ARCHITECTURE.md calls a hard
    // rule. Closing the surface removes the whole class rather than patching the
    // one command that happens to reach it today.
    params.commands = has_parent ? nullptr : commands_;
    // Same subagent rule as commands, and for the same reason (§ its own
    // comment above): a child gets no command channel, project-scoped or
    // otherwise.
    params.project_commands = has_parent ? nullptr : project_commands_;
    params.provider_factories = &providers_;
    params.provider_instance_cache = &provider_cache_;
}

void SessionRegistry::PublishSession(const std::string& session_id,
                                     const std::shared_ptr<Session>& session,
                                     SignalRouter* signal_router,
                                     const std::vector<std::string>& signal_selectors,
                                     bool signal_resume) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Signal subscriptions register HERE, inside the same critical section
    // that makes the session findable in sessions_ -- not afterward from
    // the RPC layer. ApplySignals already validated every selector before
    // the DB row/Session existed, so Subscribe here cannot fail for a
    // reason a client could have avoided; a failure is logged, not
    // surfaced, exactly like the RPC-layer code this replaces. This closes
    // the TOCTOU window where an internal signal addressed to session_id,
    // sent the instant after CreateSession's response, found no subscriber
    // even though the request that minted the id had already declared one.
    if (signal_router != nullptr && !signal_selectors.empty()) {
        if (signal_resume) signal_router->Unsubscribe(session_id);
        for (const auto& selector : signal_selectors) {
            std::string error;
            if (!signal_router->Subscribe(session, selector, error)) {
                logger_.warn("session {}: subscribing '{}' failed: {}", session_id, selector, error);
            }
        }
    }
    // The RESERVATION made at the top of CreateResolved is what guarantees
    // exclusivity -- no other construction can be in flight for this id,
    // and no live session carries it. Inserting and releasing the claim in
    // the same critical section leaves no window between the two states.
    sessions_[session_id] = session;
    reserved_.erase(session_id);
}

std::shared_ptr<Session> SessionRegistry::Find(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::size_t SessionRegistry::ChildCount(const std::string& parent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ChildCountLocked(parent_id);
}

std::size_t SessionRegistry::ChildCountLocked(const std::string& parent_id) const {
    // CONCURRENT children only -- the definition the subagent limit needs. Two
    // states free a slot: the session terminated (reaped or stream-closed), or
    // its single seeded turn finished (a finished child stays in the map until
    // its client's stream closes it, and counting it made
    // max_concurrent_subagents mean "children not yet reaped" rather than
    // "concurrent" -- a client that kept finished child tabs open would refuse
    // every new spawn).
    std::size_t count = 0;
    for (const auto& [id, session] : sessions_) {
        if (session->info().parent_session_id != parent_id) continue;
        if (session->terminated() || session->subagent_finished()) continue;
        ++count;
    }
    return count;
}

void SessionRegistry::Terminate(const std::string& session_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;
        session = it->second;
        sessions_.erase(it);
        // The session is leaving the registry for good: without this, its
        // quiescent_since_ entry would outlive it forever -- the map grows
        // once per explicitly terminated session and is only otherwise
        // pruned for sessions the reaper itself collects.
        quiescent_since_.erase(session_id);
    }
    // Terminate() joins the worker, so it runs OUTSIDE the lock: a worker
    // blocked on a permission prompt takes a moment to unwind, and holding the
    // registry lock through that would stall every other session's lookups.
    session->Terminate();
}

void SessionRegistry::TerminateAll() {
    std::vector<std::shared_ptr<Session>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, session] : sessions_) doomed.push_back(session);
        sessions_.clear();
        quiescent_since_.clear();
    }
    for (auto& session : doomed) session->Terminate();
}

void SessionRegistry::set_reaper_config(ReaperConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    reaper_config_ = std::move(config);
    initial_attach_guard_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        reaper_config_.initial_attach_guard);
    reaper_cv_.notify_all();   // a shorter guard takes effect on the next tick
}

void SessionRegistry::ReaperMain() {
    while (true) {
        std::vector<std::shared_ptr<Session>> doomed;
        // Log lines are composed under the lock but emitted after it: the
        // logger sink can block on I/O, and every other registry operation
        // would queue behind the sweep while a rotation/hung stderr drains.
        std::vector<std::string> guard_expired_logs;
        std::vector<std::string> reaped_logs;
        bool fire_idle_exit = false;
        // Set during the sweep when any session still holds the process up:
        // a never-attached session (its attach guard owns it) or a
        // detached-but-busy one (a turn in flight, a live background task,
        // subagent children). Every detached-quiescent session is
        // exit-transparent -- close-with-detach relies on it: the client is
        // gone, the turn finishes, and the idle linger below expires.
        bool any_session_holds = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Polling at a fraction of the guard keeps every deadline below
            // within one tick of its target without a timer per session.
            // Floored: the parser rejects a zero/negative
            // initial_attach_guard_seconds, but a test or direct construction
            // that bypasses it must not turn this into a 0ms wait_for, which
            // spins this thread at 100% CPU under the registry mutex.
            const auto poll_interval = std::max(initial_attach_guard_ / 4, std::chrono::milliseconds(250));
            reaper_cv_.wait_for(lock, poll_interval, [this] { return stopping_; });
            if (stopping_) return;

            const auto now = std::chrono::steady_clock::now();
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                const auto& session = it->second;
                const bool guard_expired = !session->ever_attached() &&
                                           (now - session->created_at()) > initial_attach_guard_;
                if (guard_expired) {
                    guard_expired_logs.push_back(
                        "session " + it->first +
                        ": no client attached within the guard period; terminating");
                    doomed.push_back(session);
                    it = sessions_.erase(it);
                    quiescent_since_.erase(doomed.back()->info().session_id);
                    continue;
                }
                // S5: the detached-idle reaper. ALL of these must hold, and
                // each one protects in-flight work that reaping would kill:
                //   ever_attached -- the guard above owns never-attached
                //     sessions; this rule owns everyone else.
                //   no subscribers -- an attached client IS the reason to
                //     keep the session; detachment starts the clock.
                //   idle -- no turn in flight, no queued work. A worker
                //     parked on a permission prompt counts as busy (it is
                //     mid-turn), so unattended prompts are never reaped --
                //     matching "they wait indefinitely" by default.
                //   no live background tasks -- reaping would killpg them.
                //   no subagent children -- a reaped parent loses the child's
                //     completion injection.
                const bool detached_quiescent =
                    session->ever_attached() && !session->has_subscribers() &&
                    session->idle() && session->background_tasks_live() == 0 &&
                    ChildCountLocked(it->first) == 0;
                if (!detached_quiescent) {
                    quiescent_since_.erase(it->first);
                    any_session_holds = true;
                    ++it;
                    continue;
                }
                auto since = quiescent_since_.find(it->first);
                if (since == quiescent_since_.end()) {
                    quiescent_since_.emplace(it->first, now);
                    ++it;
                    continue;
                }
                if (reaper_config_.detached_idle_ttl.count() > 0 &&
                    now - since->second >= reaper_config_.detached_idle_ttl) {
                    reaped_logs.push_back("session " + it->first + ": detached and quiescent for " +
                                          std::to_string(reaper_config_.detached_idle_ttl.count()) +
                                          "s; terminating");
                    doomed.push_back(session);
                    it = sessions_.erase(it);
                    quiescent_since_.erase(doomed.back()->info().session_id);
                } else {
                    ++it;
                }
            }

            // The process idle exit (S5, relaxed): zero attached clients AND
            // no session holding the process up, held for
            // server.idle_shutdown_seconds. The old condition demanded a
            // fully empty registry, which pinned the process for the whole
            // detached_idle_ttl whenever a client quit while any session
            // existed -- even a long-idle one. Now a session is
            // exit-transparent the moment it is detached AND quiescent (the
            // reaper's own definition above); a detached-but-busy session
            // keeps the server alive until its work finishes, and a
            // never-attached session is owned by its attach guard, not here.
            //
            // With a wired client_idle_for, "quiet" must ALSO mean "no recent
            // Ping": a live session-less client (the TUI's welcome state)
            // holds neither a stream nor a session, and only its heartbeat
            // distinguishes it from an empty world. See ReaperConfig::
            // client_idle_for. The same heartbeat keeps the server up after
            // /detach-session: the TUI is still running, its Ping still
            // fresh, so reattach stays possible.
            if (reaper_config_.idle_shutdown.count() > 0 && reaper_config_.attached_clients &&
                reaper_config_.on_idle_expired) {
                bool quiet = !any_session_holds && reaper_config_.attached_clients() == 0;
                if (quiet && reaper_config_.client_idle_for) {
                    quiet = reaper_config_.client_idle_for() >= reaper_config_.idle_shutdown;
                }
                if (quiet) {
                    if (!idle_since_) idle_since_ = now;
                    fire_idle_exit = now - *idle_since_ >= reaper_config_.idle_shutdown;
                } else {
                    idle_since_.reset();
                }
            }
        }
        for (const auto& message : guard_expired_logs) logger_.warn("{}", message);
        for (const auto& message : reaped_logs) logger_.warn("{}", message);
        for (auto& session : doomed) session->Terminate();
        // OUTSIDE the lock: RequestShutdown must not run while the registry
        // mutex is held (it joins RPC handlers that may be inside the
        // service, waiting on nothing of ours -- but the lock is cheap to
        // respect and the callback is arbitrary code).
        if (fire_idle_exit && reaper_config_.on_idle_expired) reaper_config_.on_idle_expired();

        // The tick this thread already runs is the WAL checkpoint's home;
        // see ReaperConfig::periodic_checkpoint.
        if (reaper_config_.periodic_checkpoint) reaper_config_.periodic_checkpoint();
    }
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace minimal::agent
