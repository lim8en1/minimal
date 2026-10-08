#include "minimal/agent/session_support.hpp"

#include <utility>

namespace minimal::agent {

SessionSupport::SessionSupport(common::Logger& logger) : logger_(logger) {}

void SessionSupport::Wire(SessionRegistry& sessions, session::SessionDatabase& db,
                          const AgentPresetRegistry& presets) {
    sessions_ = &sessions;
    db_ = &db;
    presets_ = &presets;
    // Built once here: HistoryFor's gone-from-the-registry fallback hands this
    // out, and it must outlive every caller's use of it -- which the service
    // (process lifetime, like its targets) guarantees.
    db_reader_ = std::make_shared<session::SessionReader>(db);
}

std::optional<SessionLlm> SessionSupport::LlmForSession(const std::string& session_id) {
    if (sessions_ == nullptr) return std::nullopt;
    const auto session = sessions_->Find(session_id);
    if (session != nullptr) {
        auto provider = session->ProviderSnapshot();
        if (provider == nullptr) return std::nullopt;
        // preset_snapshot() takes the session lock; the model name rides the preset
        // the same way it reaches BuildChatRequest, so a title goes to exactly the
        // model the session is conversing with.
        return SessionLlm{std::move(provider), session->preset_snapshot().provider.model_name};
    }

    // GONE FROM THE REGISTRY, NOT GONE FROM THE WORLD. A one-shot headless
    // turn closes its stream the instant TurnCompletedEvent lands, which can
    // terminate and erase the session BEFORE the observer thread delivers
    // OnTurnCompleted -- a queued notification must not race teardown and lose.
    // The DB row is the authoritative record of which preset and model the
    // session ran, and the resolver hands back the same shared instance the
    // session held.
    if (db_ == nullptr || presets_ == nullptr) return std::nullopt;
    const auto row = db_->GetSession(session_id);
    if (!row) return std::nullopt;
    try {
        ProviderResolver& resolver = sessions_->resolver();
        const auto model = row->model ? common::SplitModelRef(*row->model) : std::nullopt;
        const AgentPreset preset =
            resolver.ResolveModel(presets_->Resolve(row->agent_preset, {}, nullptr, model), model);
        auto provider = resolver.Acquire(preset);
        logger_.info("session {}: titling through the preset '{}' (session already ended)",
                     session_id, row->agent_preset);
        return SessionLlm{std::move(provider), preset.provider.model_name};
    } catch (const std::exception& error) {
        logger_.warn("session {}: session-support rename could not re-resolve preset '{}': {}", session_id,
                     row->agent_preset, error.what());
        return std::nullopt;
    }
}

std::optional<SessionLlm> SessionSupport::ResolveModelRef(const std::string& provider_model) {
    if (sessions_ == nullptr) return std::nullopt;
    std::string error;
    auto resolved = sessions_->resolver().ResolveLlm(provider_model, error);
    if (!resolved) {
        logger_.warn("session-support model override {}", error);
        return std::nullopt;
    }
    return SessionLlm{std::move(resolved->provider), resolved->model->provider.model_name};
}

std::string SessionSupport::ModelRefProblem(const std::string& provider_model) {
    if (sessions_ == nullptr) return "the server has no sessions to resolve it with";
    // Resolve-only: a config check must not build and cache a provider.
    return sessions_->resolver().ModelRefProblem(provider_model);
}

std::shared_ptr<const session::SessionReader> SessionSupport::HistoryFor(
    const std::string& session_id) {
    if (sessions_ == nullptr) return nullptr;
    const auto session = sessions_->Find(session_id);
    if (session != nullptr) return session->history_reader();
    // Same body as LlmForSession's fallback: the session can be erased from the
    // registry before a queued OnTurnCompleted dispatches. SessionReader is a
    // thin read-only view over the database -- the exact class and the exact
    // data the session's own reader served -- so the transcript survives the
    // session object. A session that never existed stays null (the unknown-id
    // contract), which is what separates "gone" from "never was": db_ ==
    // nullptr means unwired, not degraded.
    if (db_ == nullptr || !db_->SessionExists(session_id)) return nullptr;
    return db_reader_;
}

bool SessionSupport::RenameSession(const std::string& session_id, const std::string& name) {
    if (db_ == nullptr || sessions_ == nullptr) return false;
    if (name.empty()) {
        logger_.warn("session {}: refusing to rename to an empty name", session_id);
        return false;
    }
    // A name the user chose at creation is a decision, not a placeholder. The
    // free default-name rule protects it with its `name = ''` guard; the
    // generated-title path protects it here, by refusing -- atomically, in the
    // SQL itself (UpdateSessionNameUnlessExplicit), not via a separate
    // IsExplicitlyNamed check beforehand: a check-then-act pair here raced a
    // concurrent user /rename landing in between, letting this titler seam
    // clobber a rename that arrived after the check but before the write.
    if (!db_->UpdateSessionNameUnlessExplicit(session_id, name)) {
        logger_.info("session {}: not renamed (explicitly named, or unknown session)",
                     session_id);
        return false;
    }
    // Best effort -- a terminated session has no live object to update and no
    // attached client to notify; the database row is the authoritative record.
    if (auto session = sessions_->Find(session_id)) {
        session->SetName(name);
    }
    return true;
}

}  // namespace minimal::agent
