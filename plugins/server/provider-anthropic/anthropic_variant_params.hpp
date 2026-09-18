#pragma once

#include "minimal/llm/variant_params.hpp"

namespace minimal::provider_anthropic {

// The Anthropic "thinking" wire shape for one "@effort" variant -- relocated
// out of core (ARCHITECTURE.md, "Core vs. plugin surface"; see
// minimal::llm::VariantParamsRegistry for why this lives behind a registry
// rather than a capability method on LlmProvider). Registered under kind
// "anthropic" from plugin.cpp.
class AnthropicVariantParams : public llm::VariantParamsProvider {
public:
    std::pair<nlohmann::json, bool> DeriveParameters(
        const std::string& effort_word, std::optional<int> max_tokens) const override;
};

}  // namespace minimal::provider_anthropic
