#include "minimal/agent/provider_instance_cache.hpp"

namespace minimal::agent {

std::shared_ptr<llm::LlmProvider> ProviderInstanceCache::GetOrCreate(
    std::string_view provider_entry_name, std::string_view kind,
    const llm::ProviderConfig& config) {
    // Lock FIRST: generation_ is part of the key and was read outside it.
    // Always zero today, so harmless -- but the field exists solely for the day
    // a config reload bumps it, which is precisely the day this becomes a race
    // between the read and the bump.
    std::lock_guard<std::mutex> lock(mu_);
    const auto key = std::pair{generation_, std::string(provider_entry_name)};
    const auto it = instances_.find(key);
    if (it != instances_.end()) return it->second;

    // The factory returns a unique_ptr; the cache's copy and the caller's
    // shared_ptr share ownership from here on.
    std::shared_ptr<llm::LlmProvider> instance(factories_.Create(kind, config));
    if (instance == nullptr) return nullptr;
    instances_.emplace(key, instance);
    logger_.debug("provider instance '{}' created and cached", provider_entry_name);
    return instance;
}

}  // namespace minimal::agent
