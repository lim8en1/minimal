#pragma once

// Per-backend "@effort" variant wire-shape derivation, relocated out of core
// (ARCHITECTURE.md, "Core vs. plugin surface" -- src/server/config/providers_config.cpp
// used to hardcode `if (backend == "anthropic")` to build Anthropic's
// `{"thinking": {...}}` request object with its own effort->budget table,
// which meant a third backend with its own reasoning-effort wire shape
// needed a core change instead of a plugin, and duplicated exactly the class
// of knowledge ProviderExtension/QueryExtension was deleted from LlmProvider
// for once its one consumer moved into a plugin).
//
// A SECOND REGISTRY, not a capability query on LlmProvider: mirrors
// ModelCatalogRegistry/MetadataCatalogRegistry exactly -- kind-string-keyed
// (the SAME kind string ProviderFactoryRegistry/ModelCatalogRegistry use),
// additive, first-registration-wins with collision reporting. A backend kind
// with no registered provider here gets core's generic, backend-agnostic
// {"reasoning_effort": "<word>"} fallback -- unchanged from before this
// registry existed, and still the correct default for every OpenAI-shaped
// backend, which needs no per-kind table of its own.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/attributed_registry.hpp"

namespace minimal::llm {

class VariantParamsProvider {
public:
    virtual ~VariantParamsProvider() = default;

    // Derives {parameters, clamped} for one effort variant of this backend
    // kind -- VariantParametersFor's own return shape (a JSON object of extra
    // request parameters, and whether a value was clamped to fit max_tokens).
    // `effort_word` arrives verbatim -- every rung, "none" and "minimal"
    // included: whether and how a rung is spelled on the wire is the
    // backend's call (Anthropic omits the thinking block for "none"; a
    // LiteLLM proxy takes "xhigh" where the model advertises it). nullopt
    // hands the word to core's generic reasoning_effort mapping, which folds
    // "xhigh"/"max" to "high" and "none" to "minimal".
    // May throw std::runtime_error for a known-but-unusable effort name
    // (Anthropic's "minimal" has no budget reading) -- the caller does not
    // catch it, so the error surfaces exactly as it did when this lived in
    // core.
    //
    // `metadata` is the model's DiscoveredModel::metadata when the model came
    // from (or was enriched by) discovery, an empty object otherwise -- what
    // the backend said about the model, so a backend whose wire shape differs
    // per model (Anthropic: adaptive thinking vs a token budget) can read it
    // from the backend's own capability report instead of a per-model table.
    virtual std::optional<std::pair<nlohmann::json, bool>> DeriveParameters(
        const std::string& effort_word, std::optional<int> max_tokens,
        const nlohmann::json& metadata) const = 0;

    // The "@effort" ladder a model's metadata advertises in this backend's
    // OWN shape (Anthropic's capability report, a LiteLLM proxy's
    // /model/info row), in the order the selector lists it. nullopt: nothing
    // of this backend's in `metadata` -- core reads the cross-provider
    // catalog shapes (OpenRouter, models.dev) instead. (The default: none.)
    virtual std::optional<std::vector<std::string>> AdvertisedEfforts(
        const nlohmann::json& metadata) const {
        (void)metadata;
        return std::nullopt;
    }
};

// Kind-string -> provider. Mirrors ModelCatalogRegistry: no closed enum, the
// first registration for a kind wins, and Register returns false on a
// collision so the loader can report which two plugins claimed it.
class VariantParamsRegistry
    : public plugins::AttributedRegistry<std::shared_ptr<VariantParamsProvider>> {
public:
    // False for a null provider or a kind already taken (recorded in
    // collisions()).
    bool Register(std::string kind, std::shared_ptr<VariantParamsProvider> provider);

    // nullptr when no plugin registered a mapping for this kind -- the
    // generic reasoning_effort fallback applies.
    VariantParamsProvider* Find(std::string_view kind) const;
};

}  // namespace minimal::llm
