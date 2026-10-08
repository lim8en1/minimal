#include "minimal/agent/session_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "minimal/agent/signal_router.hpp"
#include "minimal/agent/transcript_repair.hpp"
#include "minimal/config/project_trust.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/session/sqlite_history_store.hpp"
#include "minimal/agent/policy.hpp"

namespace minimal::agent {

namespace {

template <typename T>
T& Required(T* registry, const char* name) {
    if (registry == nullptr) {
        throw std::invalid_argument(std::string("SessionRegistry needs the ") + name + " registry");
    }
    return *registry;
}

}  // namespace

SessionRegistry::SessionRegistry(const AgentPresetRegistry& presets, SessionWiring wire_session,
                                 const plugins::HostRegistries& registries,
                                 session::SessionDatabase& db, LoopConfig loop_config,
                                 common::Logger& logger, Options options)
    : presets_(presets),
      wire_session_(wire_session ? std::move(wire_session)
                                 : throw std::invalid_argument(
                                       "SessionRegistry needs a session wiring step")),
      registries_(registries),
      tools_(Required(registries.tools, "tool")),
      resolver_(presets.providers(), Required(registries.providers, "provider factory"), logger),
      observers_(Required(registries.observers, "session observer")),
      db_(db),
      history_reader_(std::make_shared<session::SessionReader>(db_)),
      loop_config_(loop_config),
      prompt_timeout_(options.prompt_timeout),
      logger_(logger),
      initial_attach_guard_(std::chrono::duration_cast<std::chrono::milliseconds>(
          options.reaper.initial_attach_guard)),
      reaper_config_(options.reaper) {
    project_commands_ = options.project_commands;
    signal_router_ = options.signal_router;
    if (!options.small_model.empty()) ApplySmallModel(options.small_model);
    // Started last, once every member it reads exists.
    reaper_ = std::make_unique<common::PeriodicTask>([this] { return ReapInterval(); },
                                                     [this] { ReapOnce(); });
}

void SessionRegistry::ApplySmallModel(const std::string& provider_model) {
    std::string error;
    auto resolved = resolver_.ResolveLlm(provider_model, error);
    if (!resolved) {
        common::DefaultLogger().warn(
            "settings.small_model {}; compaction uses each session's own model", error);
        return;
    }
    const config::ResolvedModel& model = *resolved->model;
    LoopConfig::SmallModel small;
    small.provider = std::move(resolved->provider);
    small.model_name = model.provider.model_name;
    small.parameters = model.parameters;
    small.max_tokens = model.max_tokens;
    small.context_window_tokens = model.context_window_tokens;
    loop_config_.small_model = std::move(small);
}

SessionRegistry::~SessionRegistry() {
    StopReaper();
    TerminateAll();
}

// Idempotent: PeriodicTask::Stop is.
void SessionRegistry::StopReaper() {
    if (reaper_ != nullptr) reaper_->Stop();
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
        presets_.Resolve(params.agent_preset, params.project_root, nullptr,
                         params.model_override);
    return CreateResolved(std::move(params), std::move(preset));
}

std::shared_ptr<Session> SessionRegistry::ForkSession(
    const std::string& source_session_id, const std::string& name, const std::string& instruction,
    const std::optional<config::ModelRef>& model, const std::optional<std::string>& agent_preset,
    std::optional<tools::PermissionRunMode> run_mode) {
    // The store mints the child row and copies the transcript prefix in one
    // transaction -- the same "the DATABASE mints the id" ordering every other
    // creation path follows. The fork transaction copies the source's
    // persisted model/subagent_model columns verbatim, and the returned row
    // reports them.
    session::SessionSummary row = db_.ForkSession(source_session_id, name, agent_preset);

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
    params.name = row.name;  // "fork of ..." or empty: default naming from the first user text
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
        if (model) {
            // Persist the EXPLICIT override durably: db_.ForkSession only ever
            // copies the source's own model column into the new row, with no
            // parameter for a request-level override. Without this, the
            // override lives only in params.model_override for this
            // process's lifetime -- a restart resumes the forked session by
            // re-reading the row, which still holds the source's model,
            // silently discarding the user's explicit fork-time choice.
            // Inside this try so a failure here rolls back the row exactly
            // like a failed Create() does.
            db_.UpdateSessionModel(row.session_id, model->str());
        }
        std::shared_ptr<Session> session = Create(std::move(params));
        // The instruction is the fork's first user turn, queued here so the
        // fork and its divergence are one step -- no attach-then-send race.
        // A refusal is already answered on the session's own queue
        // (SubmitTurn's contract); the fork itself stands either way.
        if (!instruction.empty()) session->SubmitTurn(instruction);
        return session;
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
    SignalRouter* const signal_router = params.signal_router;
    const std::vector<std::string> signal_selectors = std::move(params.signal_selectors);
    const bool resume = params.resume;

    // S5.3: same funnel, same reasoning -- a producer that built its own
    // permission_checker (an advised session) owns its own timeout policy, so
    // this only takes effect on the default WirePermissionChecker/
    // WireConfirmationBroker Session builds when params.permission_checker is
    // null. Overwriting an explicit non-zero value a producer already set
    // would defeat that override, so only the funnel default applies.
    if (params.unattended_prompt_timeout == std::chrono::milliseconds::zero()) {
        params.unattended_prompt_timeout = prompt_timeout_;
    }
    // The transport, the (possibly advisor-composed) permission checker and
    // the confirmation/ask brokers. AFTER unattended_prompt_timeout resolves
    // just above -- the defaults are built against it -- and here because this
    // is the one funnel every session (top-level, resume, subagent) passes.
    wire_session_(params);
    AssembleContext(params, preset);

    // The user's model outranks the preset's. Applied here rather than inside
    // the preset registry because it is a property of this SESSION, not of the
    // preset -- two sessions on one preset can be running different models. In
    // CreateResolved rather than Create so a SUBAGENT spawned with an inherited
    // preset honours it too.
    preset = resolver_.ResolveModel(std::move(preset), params.model_override);

    LoopConfig loop_config = loop_config_;
    if (max_turns_override.has_value()) loop_config.max_turns = *max_turns_override;
    // Track O: the same funnel property wires the hook dispatcher. A producer
    // that set one explicitly (a test with its own) wins; everyone else gets
    // the process-wide one without knowing it exists. One field now, not two
    // (Phase 5 step 8): Session reads this LoopConfig, not a second copy on
    // SessionParams.
    if (loop_config.hooks == nullptr) loop_config.hooks = registries_.hooks;

    auto provider = resolver_.Acquire(preset);

    // Track M (docs/EXTENSIBILITY.md §2.1): the read side of the transcript.
    // Set here because this is the one funnel every session passes through --
    // top-level and SubagentManager spawns alike -- so a child's window is
    // bound to the child's own id with no extra code, and its tools see its
    // transcript only. Sessions built outside the registry (plain tests on
    // fake stores) leave it null, and ctx.history is null with it. The SAME
    // registry-lifetime reader every session shares -- see history_reader_.
    params.history_reader = history_reader_;

    // Process-wide wiring reaches Session's constructor on `params`, so it is
    // never null while the worker thread runs.
    ApplyConstructorWiring(params);

    std::optional<llm::Message> pending_repair;
    ResumeSeed seed = resume ? LoadResumeSeed(session_id, pending_repair) : ResumeSeed{};

    auto store = std::make_unique<session::SqliteHistoryStore>(db_, session_id);
    session::SqliteHistoryStore* const store_handle = store.get();  // owned by the Session below
    // loop_config, not loop_config_: CreateResolved may have overridden max_turns
    // for a subagent, whose whole life is one bounded turn.
    auto session = std::make_shared<Session>(std::move(params), std::move(preset),
                                             std::move(provider), std::move(store), tools_,
                                             loop_config, observers_, logger_, std::move(seed));
    // The synthesized resume repair (answers for tool_use blocks the crashed
    // run left unpaired) is persisted HERE: after the Session exists, so a
    // construction failure (unresolvable preset/model) leaves nothing
    // committed to a transcript for a session that was never created -- and
    // BEFORE it is published. Published first, an incoming turn, signal wake
    // or second client could persist ahead of the repair row, leaving
    // [assistant(tool_use), user(turn), user(results)] -- a permanent 400 on
    // every later request. Nothing can address the session until
    // PublishSession, and going through the store keeps its seq accounting
    // (last_appended_seq) in step with the row. A failure here unwinds: the unpublished
    // Session is destroyed (its worker joined) and `guard` releases the id.
    if (pending_repair) {
        store_handle->Append(*pending_repair, 0, session::MessageAttribution{});
    }

    // Made findable last: the session is fully wired (constructor-injected)
    // and constructed before PublishSession's locked block registers signal
    // subscriptions and inserts it into sessions_ -- an internal signal or
    // command addressed to this session_id must never be able to observe it
    // before that.
    PublishSession(session_id, session, signal_router, signal_selectors, resume);

    AnnounceIgnoredProjectContent(*session);

    guard.committed = true;
    return session;
}

void SessionRegistry::AnnounceIgnoredProjectContent(Session& session) const {
    // A top-level session in a project that ships its own agents/skills/commands
    // that are being IGNORED (project_local.mode "trusted", directory not
    // trusted) says so once. Silence would read as "the repo has no agents"; the
    // notice names the one command that changes it. Subagents never announce.
    if (!session.info().parent_session_id.empty()) return;
    const config::ProjectTrust* trust = presets_.project_trust();
    if (trust == nullptr || trust->mode() != config::ProjectLocalMode::kTrusted) return;
    const std::filesystem::path& root = session.info().project_root;
    if (trust->Allows(root)) return;
    const config::ProjectLocalInventory found = trust->Inspect(root);
    if (found.empty()) return;
    session.EmitOrWarn(
        SessionEvent{SessionError{"project-local " + found.Describe() +
                                  " in this directory are ignored until you run /trust-directory"}},
        "project trust notice");
}

void SessionRegistry::AssembleContext(SessionParams& params, const AgentPreset& preset) const {
    // The session's constant context tail, rendered once here -- the funnel
    // creation, resume and subagent spawns share -- and frozen for the
    // session: plugin sections (project instructions, ...) and core's signal-
    // channel instructions, ordered by priority (0 first; ties keep
    // registration order). AgentLoop renders it after the preset body on every
    // request and knows nothing of where it came from.
    std::vector<RenderedContextSection> sections;
    if (registries_.context_contributors != nullptr) {
        ContextRequest request;
        request.session_id = params.session_id;
        request.parent_session_id = params.parent_session_id;
        request.project_root = params.project_root;
        request.preset_name = preset.name;
        request.is_subagent = !params.parent_session_id.empty();
        request.resume = params.resume;
        sections = registries_.context_contributors->RenderSections(request);
    }
    if (!params.signal_context.empty()) {
        sections.push_back({kSignalContextPriority, std::move(params.signal_context)});
        params.signal_context.clear();
    }
    params.project_context = JoinContextSections(std::move(sections));
}

ResumeSeed SessionRegistry::LoadResumeSeed(const std::string& session_id,
                                           std::optional<llm::Message>& pending_repair) {
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
        // The loop drops the thinking of the messages the fold kept, as the
        // live fold did (AgentLoop::DropStaleThinking).
        seed.kept_through_seq = checkpoint->at_seq;
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
    // append, but not from HERE -- see pending_repair's doc comment) rather
    // than refusing to resume: cancellation is ordinary, and the transcript
    // stays an exact, valid prefix either way. The repair appends at
    // MAX(seq)+1, so a checkpoint boundary (which only covers earlier seqs)
    // is unaffected by it.
    if (auto repair = synthesize_missing_tool_results(seed.history)) {
        logger_.info(
            "session {}: stored transcript ends mid tool call; appending synthesized "
            "error results before resuming",
            session_id);
        seed.history.push_back(*repair);
        pending_repair = std::move(repair);
    }
    return seed;
}

void SessionRegistry::ApplyConstructorWiring(SessionParams& params) {
    params.environment.presets = &presets_;
    params.environment.compaction_strategies = registries_.compaction_strategies;
    params.environment.tool_sources = registries_.tool_sources;
    params.environment.provider_resolver = &resolver_;
    // A SUBAGENT GETS A READ-ONLY COMMAND SURFACE. Commands are a user control
    // channel, and a child has no user: its client attaches to observe, and the
    // spawn was driven by the parent's model rather than by a person with a
    // prompt. But the observing client still benefits from the harmless half of
    // the registry (/cost, /help <cmd>) -- commands that touch no session state
    // and may run beside a live turn by definition.
    //
    // The registry is therefore wired for children too; what a child never gets
    // is a MUTATING dispatch. That is the escape hatch closure: /agent re-
    // resolves a preset from disk, and a freshly resolved preset has
    // is_subagent = false and its own full allowed_tools -- so a child running
    // `/agent build` would get spawn_subagent back and reopen the recursion
    // ARCHITECTURE.md calls a hard rule. /agent is not ReadOnly(), so the
    // subagent guard (SessionServiceImpl::DispatchCommand reader-side, and
    // Session::ExecuteCommand as defense in depth) refuses it and its whole
    // class. The guard is derived from the command object's own ReadOnly()
    // answer, so a future read-only command works on a child for free and a
    // future mutating one is refused without this file learning about it.
    params.environment.commands = registries_.commands;
    params.environment.project_commands = project_commands_;
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
        // Same reasoning for the signal router's subscription entry: absent
        // this, subs_ only ever shrank on a resume, so a session that
        // terminates normally leaked its entry for the life of the process.
        if (signal_router_ != nullptr) signal_router_->Unsubscribe(session_id);
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
        for (auto& [id, session] : sessions_) {
            doomed.push_back(session);
            if (signal_router_ != nullptr) signal_router_->Unsubscribe(id);
        }
        sessions_.clear();
        quiescent_since_.clear();
    }
    for (auto& session : doomed) session->Terminate();
}

std::unordered_map<std::string, std::size_t> SessionRegistry::LiveChildCountsLocked() const {
    // One O(n) pass: ChildCountLocked rescans the whole map, and calling it
    // once per session made a sweep O(n^2) under the held mutex. A parent
    // absent from the result has zero live children.
    std::unordered_map<std::string, std::size_t> counts;
    for (const auto& [id, session] : sessions_) {
        const auto& parent = session->info().parent_session_id;
        if (parent.empty() || session->terminated() || session->subagent_finished()) continue;
        ++counts[parent];
    }
    return counts;
}

// ALL of these must hold, and each one protects in-flight work that reaping
// would kill:
//   ever_attached -- the attach guard owns never-attached sessions; this rule
//     owns everyone else.
//   no subscribers -- an attached client IS the reason to keep the session;
//     detachment starts the clock.
//   idle -- no turn in flight, no queued work. A worker parked on a
//     permission prompt counts as busy (it is mid-turn), so unattended prompts
//     are never reaped.
//   no live background tasks -- reaping would killpg them.
//   no subagent children -- a reaped parent loses the child's completion
//     injection.
bool SessionRegistry::DetachedQuiescentLocked(
    const std::string& session_id, const Session& session,
    const std::unordered_map<std::string, std::size_t>& live_child_counts) const {
    return session.ever_attached() && !session.has_subscribers() && session.idle() &&
           session.background_tasks_live() == 0 &&
           live_child_counts.find(session_id) == live_child_counts.end();
}

bool SessionRegistry::AnySessionHolds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto live_child_counts = LiveChildCountsLocked();
    for (const auto& [id, session] : sessions_) {
        // A never-attached session is owned by its attach guard, and holds
        // the process until the guard reaps it.
        if (!session->ever_attached()) return true;
        if (!DetachedQuiescentLocked(id, *session, live_child_counts)) return true;
    }
    return false;
}

std::chrono::milliseconds SessionRegistry::ReapInterval() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // A fraction of the guard keeps every deadline within one tick of its
    // target without a timer per session. Floored: a test or direct
    // construction that bypasses the settings parser must not turn this into
    // a 0ms wait that spins the thread under the registry mutex.
    return std::max(initial_attach_guard_ / kReapTicksPerAttachGuard, kMinReapInterval);
}

void SessionRegistry::ReapOnce() {
    std::vector<std::shared_ptr<Session>> doomed;
    // Log lines are composed under the lock but emitted after it: the logger
    // sink can block on I/O, and every other registry operation would queue
    // behind the sweep.
    std::vector<std::string> logs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto live_child_counts = LiveChildCountsLocked();
        const auto now = std::chrono::steady_clock::now();
        const auto reap = [&](auto& it, std::string message) {
            logs.push_back(std::move(message));
            doomed.push_back(it->second);
            quiescent_since_.erase(it->first);
            if (signal_router_ != nullptr) signal_router_->Unsubscribe(it->first);
            it = sessions_.erase(it);
        };
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const auto& session = it->second;
            // The attach guard exists for TOP-LEVEL sessions: a client that
            // created one and never opened a stream. A subagent is owned by
            // its parent, not by a client -- a blocking spawn waits on it with
            // nobody attached, and killing it mid-turn at the guard deadline
            // fails the parent's tool call ("subagent was terminated") or
            // silently loses a non-blocking child's result. A child is
            // collected once it has FINISHED (its result is delivered by then).
            const bool is_child = !session->info().parent_session_id.empty();
            const bool guard_applies = !is_child || session->subagent_finished();
            if (guard_applies && !session->ever_attached() &&
                (now - session->created_at()) > initial_attach_guard_) {
                reap(it, "session " + it->first +
                             (is_child ? ": finished subagent was never attached; collecting"
                                       : ": no client attached within the guard period; "
                                         "terminating"));
                continue;
            }
            if (!DetachedQuiescentLocked(it->first, *session, live_child_counts)) {
                quiescent_since_.erase(it->first);
                ++it;
                continue;
            }
            const auto since = quiescent_since_.find(it->first);
            if (since == quiescent_since_.end()) {
                quiescent_since_.emplace(it->first, now);
                ++it;
                continue;
            }
            if (reaper_config_.detached_idle_ttl.count() > 0 &&
                now - since->second >= reaper_config_.detached_idle_ttl) {
                reap(it, "session " + it->first + ": detached and quiescent for " +
                             std::to_string(reaper_config_.detached_idle_ttl.count()) +
                             "s; terminating");
            } else {
                ++it;
            }
        }
    }
    for (const auto& message : logs) logger_.warn("{}", message);
    for (auto& session : doomed) session->Terminate();
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace minimal::agent
