#pragma once

#include "minimal/llm/variant_params.hpp"

namespace minimal::provider_openai {

// The "@effort" ladder and wire word for an OpenAI-shape backend. Registered
// under kind "openai" from plugin.cpp.
//
// The backend's own metadata shape is a LiteLLM proxy's /model/info row,
// stored under "model_info" by an opted-in discovery (OpenAiModelCatalog):
// `supports_reasoning` means the proxy maps reasoning_effort's standard
// low/medium/high for the model, and per-level flags add the model-specific
// rungs (none/minimal/xhigh/max) or, set false, withdraw a standard one.
// Flags present-but-null -- the proxy's spelling for "not a reasoning model"
// -- say nothing.
//
// A rung the model_info row advertises is sent VERBATIM (reasoning_effort:
// "xhigh", "none"): the proxy said the model takes it, and core's generic
// fold (xhigh/max -> high, none -> minimal) would send a different request
// than the variant names -- a duplicate of @high, or "minimal" to a model
// that only takes "none". Everything else gets that generic mapping.
class OpenAiVariantParams : public llm::VariantParamsProvider {
public:
    std::optional<std::pair<nlohmann::json, bool>> DeriveParameters(
        const std::string& effort_word, std::optional<int> max_tokens,
        const nlohmann::json& metadata) const override;
    std::optional<std::vector<std::string>> AdvertisedEfforts(
        const nlohmann::json& metadata) const override;
};

}  // namespace minimal::provider_openai
