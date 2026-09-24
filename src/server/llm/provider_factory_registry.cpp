#include "minimal/llm/provider_factory_registry.hpp"

#include <utility>

namespace minimal::llm {

bool ProviderFactoryRegistry::Register(std::string kind, Factory factory) {
    if (!factory) return false;
    return AttributedRegistry::Register(std::move(kind), std::move(factory));
}

std::unique_ptr<LlmProvider> ProviderFactoryRegistry::Create(std::string_view kind,
                                                              const ProviderConfig& config) const {
    const Factory* factory = FindEntry(kind);
    return factory == nullptr ? nullptr : (*factory)(config);
}

}  // namespace minimal::llm
