#include "minimal/llm/variant_params.hpp"

namespace minimal::llm {

bool VariantParamsRegistry::Register(std::string kind,
                                     std::shared_ptr<VariantParamsProvider> provider) {
    if (!provider) return false;
    return AttributedRegistry::Register(std::move(kind), std::move(provider));
}

VariantParamsProvider* VariantParamsRegistry::Find(std::string_view kind) const {
    const auto* entry = FindEntry(kind);
    return entry == nullptr ? nullptr : entry->get();
}

}  // namespace minimal::llm
