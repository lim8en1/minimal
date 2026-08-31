#include "minimal/agent/session_support.hpp"

#include <utility>

namespace minimal::agent {

SessionSupport::SessionSupport(common::Logger& logger) : logger_(logger) {}

void SessionSupport::Wire(SessionRegistry& sessions, session::SessionDatabase& db,
                          const AgentPresetRegistry& presets) {
    sessions_ = &sessions;
    db_ = &db;
    presets_ = &presets;
}

std::optional<SessionLlm> SessionSupport::LlmForSession(const std::string& session_id) {
    if (sessions_ == nullptr) return std::nullopt;
    const auto session = sessions_->Find(session_id);
    if (session == nullptr) return std::nullopt;
    auto provider = session->ProviderSnapshot();
    if (provider == nullptr) return std::nullopt;
    // preset_snapshot() takes the session lock; the model name rides the preset
    // the same way it reaches BuildChatRequest, so a title goes to exactly the
    // model the session is conversing with.
    return SessionLlm{std::move(provider), session->preset_snapshot().provider.model_name};
}

std::optional<SessionLlm> SessionSupport::ResolveModelRef(const std::string& provider_model) {
    if (sessions_ == nullptr || presets_ == nullptr) return std::nullopt;
    const auto ref = common::ParseModelRef(provider_model);
    if (!ref) {
        logger_.warn("session titler model override '{}' is not a <provider>/<model> reference",
                     provider_model);
        return std::nullopt;
    }
    const auto resolved = presets_->providers().Resolve(*ref);
    if (resolved == nullptr) {
        logger_.warn("session titler model override '{}' resolves to nothing in providers.json",
                     provider_model);
        return std::nullopt;
    }
    // The cache, not a fresh construct: the same instance the session using
    // this entry holds. Providers are stateless by contract, so sharing costs
    // nothing and avoids a second connection to the same backend.
    auto provider = sessions_->provider_cache().GetOrCreate(ref->provider, resolved->backend,
                                                            resolved->provider);
    if (provider == nullptr) {
        logger_.warn("session titler model override '{}' failed to construct a provider",
                     provider_model);
        return std::nullopt;
    }
    return SessionLlm{std::move(provider), resolved->provider.model_name};
}

std::shared_ptr<const session::SessionReader> SessionSupport::HistoryFor(
    const std::string& session_id) {
    if (sessions_ == nullptr) return nullptr;
    const auto session = sessions_->Find(session_id);
    if (session == nullptr) return nullptr;
    return session->history_reader();
}

bool SessionSupport::RenameSession(const std::string& session_id, const std::string& name) {
    if (db_ == nullptr || sessions_ == nullptr) return false;
    if (name.empty()) {
        logger_.warn("session {}: refusing to rename to an empty name", session_id);
        return false;
    }
    // A name the user chose at creation is a decision, not a placeholder. The
    // free default-name rule protects it with its `name = ''` guard; the
    // generated-title path protects it here, by refusing. The refusal is
    // logged, not silent: a titler configured over an explicitly-named session
    // would otherwise look like a broken plugin.
    if (db_->IsExplicitlyNamed(session_id)) {
        logger_.info("session {}: keeps its explicit name; titler skipped", session_id);
        return false;
    }
    if (!db_->UpdateSessionName(session_id, name)) return false;
    // Best effort -- a terminated session has no live object to update and no
    // attached client to notify; the database row is the authoritative record.
    if (auto session = sessions_->Find(session_id)) {
        session->SetName(name);
    }
    return true;
}

}  // namespace minimal::agent
