#pragma once

#include "minimal/llm/variant_params.hpp"

namespace minimal::provider_anthropic {

// The Anthropic "thinking" wire shape for one "@effort" variant -- relocated
// out of core (ARCHITECTURE.md, "Core vs. plugin surface"; see
// minimal::llm::VariantParamsRegistry for why this lives behind a registry
// rather than a capability method on LlmProvider). Registered under kind
// "anthropic" from plugin.cpp.
//
// The shape is chosen per model from the /v1/models `capabilities` tree the
// catalog stores in the model's metadata -- no per-model table:
//   - adaptive thinking supported -> {"thinking": {"type": "adaptive",
//     "display": "summarized"}, "output_config": {"effort": <word>}}
//   - only budgeted thinking ("enabled") -> {"thinking": {"type": "enabled",
//     "budget_tokens": N}}, plus output_config.effort when the model takes it
//   - no thinking at all -> output_config.effort alone, when supported
// A model with no capability report (a static model discovery never saw) is
// treated as current: adaptive, every effort word. An effort word the model
// does not take steps DOWN to the nearest one it does.
class AnthropicVariantParams : public llm::VariantParamsProvider {
public:
    std::pair<nlohmann::json, bool> DeriveParameters(const std::string& effort_word,
                                                     std::optional<int> max_tokens,
                                                     const nlohmann::json& metadata) const override;
};

}  // namespace minimal::provider_anthropic
