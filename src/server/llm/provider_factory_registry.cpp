#include "minimal/llm/provider_factory_registry.hpp"

#include <algorithm>
#include <utility>

namespace minimal::llm {

bool ProviderFactoryRegistry::Register(std::string kind, Factory factory) {
    if (!factory) return false;
    auto [it, inserted] = factories_.try_emplace(std::move(kind), std::move(factory));
    (void)it;
    return inserted;
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
