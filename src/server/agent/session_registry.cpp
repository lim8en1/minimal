#include "minimal/agent/session_registry.hpp"

#include <stdexcept>
#include <utility>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/config/project_context.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/session/sqlite_history_store.hpp"

namespace minimal::agent {
namespace {

// Same shape as the preset registry's message: a miss is answered by showing
// what exists, because it is nearly always a typo or an unconfigured provider.
std::string DescribeModels(const config::ProvidersConfig& providers) {
    const auto refs = providers.ModelRefs();
    if (refs.empty()) return "no models at all (providers.json is empty)";
    std::string out;
    for (const auto& ref : refs) {
        if (!out.empty()) out += ", ";
        out += ref;
    }
    return out;
}

// Miss reporting (track N): a typo and a dead catalog must read differently.
// Silent when discovery was never attempted or succeeded -- the latter means
// the model is genuinely not served, which the listing above already says.
std::string DescribeDiscovery(const config::ProvidersConfig& providers,
                              const std::string& provider_name) {
    const auto status = providers.discovery_status(provider_name);
    if (!status.attempted || status.ok) return {};
    return "; discovery for provider '" + provider_name + "' failed: " + status.error;
}

}  // namespace

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
      loop_config_(loop_config),
      logger_(logger),
      initial_attach_guard_(initial_attach_guard) {
    reaper_ = std::thread([this] { ReaperMain(); });
}

SessionRegistry::~SessionRegistry() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    reaper_cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();
    TerminateAll();
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
    AgentPreset preset = presets_.Resolve(params.agent_preset);
    return CreateResolved(std::move(params), std::move(preset));
}

std::shared_ptr<Session> SessionRegistry::CreateResolved(SessionParams params,
                                                          AgentPreset preset,
                                                          std::optional<int> max_turns_override) {
    // Read before params is moved from.
    const std::string params_parent = params.parent_session_id;
    const std::filesystem::path project_root = params.project_root;

    // PROJECT CONTEXT (AGENTS.md/CLAUDE.md), loaded HERE -- in the funnel every
    // session passes through: top-level creation, resume, and SubagentManager
    // spawns alike. The file is a property of the session's project_root, which
    // no preset knows; the rendered section rides SessionParams into the loop,
    // which appends it after the preset body on every request. Absence is the
    // normal state and silent; a file that exists but cannot be read is worth
    // one warning, since the operator wrote instructions and got silence.
    std::string error;
    if (auto context = config::LoadProjectContext(project_root, project_context_, &error)) {
        params.project_context = config::RenderProjectContextSection(*context);
        logger_.info("session {}: loaded project context from {}", params.session_id,
                     context->source_path.string());
    } else if (!error.empty()) {
        logger_.warn("session {}: {}", params.session_id, error);
    }
    // The user's model outranks the preset's. Applied here rather than inside
    // the preset registry because it is a property of this SESSION, not of the
    // preset -- two sessions on one preset can be running different models. In
    // CreateResolved rather than Create so a SUBAGENT spawned with an inherited
    // preset honours it too.
    if (params.model_override) {
        const auto model = presets_.providers().Resolve(*params.model_override);
        if (model == nullptr) {
            throw std::runtime_error("unknown model '" + params.model_override->str() +
                                     "'; providers.json defines " +
                                     DescribeModels(presets_.providers()) +
                                     DescribeDiscovery(presets_.providers(),
                                                       params.model_override->provider));
        }
        ApplyModel(preset, *model);
    }

    LoopConfig loop_config = loop_config_;
    if (max_turns_override.has_value()) loop_config.max_turns = *max_turns_override;

    // The kind string is what selects a factory. A preset can only reach this
    // point with a provider entry that parsed, so an unregistered kind means a
    // plugin is missing or disabled -- worth naming explicitly, because the
    // symptom otherwise is a session that fails on its first turn for no stated
    // reason.
    if (!providers_.Has(preset.provider_kind)) {
        throw std::runtime_error("agent preset '" + params.agent_preset +
                                 "' needs provider kind '" + preset.provider_kind +
                                 "', which no loaded plugin registered");
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

    const std::string session_id = params.session_id;

    // Track M (docs/EXTENSIBILITY.md §2.1): the read side of the transcript.
    // Set here because this is the one funnel every session passes through --
    // top-level and SubagentManager spawns alike -- so a child's window is
    // bound to the child's own id with no extra code, and its tools see its
    // transcript only. Sessions built outside the registry (plain tests on
    // fake stores) leave it null, and ctx.history is null with it.
    params.history_reader = std::make_shared<session::SessionReader>(db_);

    // Track O: the same funnel property wires the hook dispatcher. A producer
    // that set one explicitly (a test with its own) wins; everyone else gets
    // the process-wide one without knowing it exists.
    if (params.hooks == nullptr) params.hooks = hooks_;

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
    std::vector<llm::Message> seeded_history;
    std::optional<std::int64_t> seed_first_persisted_seq;
    bool seed_head_is_summary = false;
    if (params.resume) {
        const session::SessionReader reader(db_);
        const auto checkpoint = db_.LoadCompactionCheckpoint(session_id);
        if (checkpoint) {
            seeded_history.push_back(checkpoint->summary);
            for (auto& stored : reader.LoadAfter(session_id, checkpoint->boundary_seq)) {
                if (!seed_first_persisted_seq) seed_first_persisted_seq = stored.seq;
                seeded_history.push_back(std::move(stored.message));
            }
            seed_head_is_summary = true;
            if (!seed_first_persisted_seq) {
                // Empty tail: the boundary is still exactly where the persisted
                // portion will resume on the next append.
                seed_first_persisted_seq = checkpoint->boundary_seq + 1;
            }
            logger_.info("session {}: resuming from compaction checkpoint (covers seq <= {})",
                         session_id, checkpoint->boundary_seq);
        } else {
            for (auto& stored : reader.LoadAll(session_id)) {
                if (!seed_first_persisted_seq) seed_first_persisted_seq = stored.seq;
                seeded_history.push_back(std::move(stored.message));
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
        if (auto repair = synthesize_missing_tool_results(seeded_history)) {
            logger_.info(
                "session {}: stored transcript ends mid tool call; appending synthesized "
                "error results before resuming",
                session_id);
            db_.AppendMessage(session_id, *repair, 0);
            seeded_history.push_back(std::move(*repair));
        }
    }

    auto store = std::make_unique<session::SqliteHistoryStore>(db_, session_id);
    // loop_config, not loop_config_: CreateResolved may have overridden max_turns
    // for a subagent, whose whole life is one bounded turn.
    auto session = std::make_shared<Session>(std::move(params), std::move(preset),
                                             std::move(provider), std::move(store), tools_,
                                             loop_config, observers_, logger_,
                                             std::move(seeded_history), seed_first_persisted_seq,
                                             seed_head_is_summary);
    // Wired before the session is findable, so the first command a client sends
    // cannot arrive at a session that has no registry yet.
    session->set_presets(&presets_);
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
    session->set_commands(params_parent.empty() ? commands_ : nullptr);
    session->set_provider_factories(&providers_);
    session->set_provider_instance_cache(&provider_cache_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Backstop, checked under the lock: the service layer refuses an
        // already-open session via Find() before reaching here, but two
        // concurrent resumes of the same id could both pass that check. A
        // silent overwrite would leave two live Sessions appending to one
        // transcript. For a fresh session the database-minted ULID makes a
        // collision implausible, so this only matters for resume.
        if (sessions_.count(session_id)) {
            throw std::runtime_error("session '" + session_id + "' is already open");
        }
        sessions_[session_id] = session;
    }
    reaper_cv_.notify_all();
    return session;
}

std::shared_ptr<Session> SessionRegistry::Find(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::size_t SessionRegistry::ChildCount(const std::string& parent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // LIVE children only. A finished child stays in the map until its client's
    // stream closes it, and counting it made max_concurrent_subagents mean
    // "children not yet reaped" rather than "concurrent" -- a client that kept
    // finished child tabs open would refuse every new spawn.
    std::size_t count = 0;
    for (const auto& [id, session] : sessions_) {
        if (session->info().parent_session_id == parent_id && !session->terminated()) ++count;
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
    }
    for (auto& session : doomed) session->Terminate();
}

void SessionRegistry::ReaperMain() {
    while (true) {
        std::vector<std::shared_ptr<Session>> doomed;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Polling at a fraction of the guard keeps the reap within one tick
            // of the deadline without a timer per session.
            reaper_cv_.wait_for(lock, initial_attach_guard_ / 4, [this] { return stopping_; });
            if (stopping_) return;

            const auto now = std::chrono::steady_clock::now();
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                const auto& session = it->second;
                const bool expired = !session->ever_attached() &&
                                     (now - session->created_at()) > initial_attach_guard_;
                if (expired) {
                    logger_.warn(
                        "session {}: no client attached within the guard period; terminating",
                        it->first);
                    doomed.push_back(session);
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& session : doomed) session->Terminate();
    }
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace minimal::agent
