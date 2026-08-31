#include "minimal/llm/provider_factory_registry.hpp"

#include <algorithm>
#include <utility>

namespace minimal::llm {

bool ProviderFactoryRegistry::Register(std::string kind, Factory factory) {
    if (!factory) return false;
    const std::string key = kind;  // read before the move below
    auto [it, inserted] = factories_.try_emplace(std::move(kind), std::move(factory));
    (void)it;
    if (inserted) {
        registrant_[key] = attribution_;
        return true;
    }
    // Recorded, not just refused. The loser is dropped either way -- the
    // collision is fatal upstream, so which object holds the slot is moot -- but
    // a silent drop made the winner depend on load order with nothing to point
    // an operator at.
    const auto first = registrant_.find(key);
    collisions_.push_back(Collision{key, first != registrant_.end() ? first->second : std::string{},
                                    attribution_});
    return false;
}

bool ProviderFactoryRegistry::Has(std::string_view kind) const {
    return factories_.find(std::string(kind)) != factories_.end();
}

std::unique_ptr<LlmProvider> ProviderFactoryRegistry::Create(std::string_view kind,
                                                              const ProviderConfig& config) const {
    auto it = factories_.find(std::string(kind));
    if (it == factories_.end()) return nullptr;
    return it->second(config);
}

std::vector<std::string> ProviderFactoryRegistry::kinds() const {
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& [kind, _] : factories_) out.push_back(kind);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace minimal::llm
