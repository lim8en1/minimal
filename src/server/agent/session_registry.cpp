#include "minimal/agent/session_registry.hpp"

#include <stdexcept>
#include <utility>

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
    // the lookup read a moved-from name.
    AgentPreset preset = presets_.Resolve(params.agent_preset);
    return CreateResolved(std::move(params), std::move(preset));
}

std::shared_ptr<Session> SessionRegistry::CreateResolved(SessionParams params,
                                                          AgentPreset preset,
                                                          std::optional<int> max_turns_override) {
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
    auto provider = providers_.Create(preset.provider_kind, preset.provider);
    if (provider == nullptr) {
        throw std::runtime_error("provider kind '" + preset.provider_kind +
                                 "' failed to construct");
    }

    const std::string session_id = params.session_id;
    auto store = std::make_unique<session::SqliteHistoryStore>(db_, session_id);
    auto session = std::make_shared<Session>(std::move(params), std::move(preset),
                                             std::move(provider), std::move(store), tools_,
                                             loop_config, observers_, logger_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
    std::size_t count = 0;
    for (const auto& [id, session] : sessions_) {
        if (session->info().parent_session_id == parent_id) ++count;
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
