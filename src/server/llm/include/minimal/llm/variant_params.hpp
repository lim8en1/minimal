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

namespace minimal::llm {

class VariantParamsProvider {
public:
    virtual ~VariantParamsProvider() = default;

    // Derives {parameters, clamped} for one effort variant of this backend
    // kind -- VariantParametersFor's own return shape (a JSON object of extra
    // request parameters, and whether a value was clamped to fit max_tokens).
    // `effort_word` arrives already normalized by the caller (core folds
    // "xhigh"/"max" to "high" once, before dispatch, so it cannot drift
    // between a plugin's mapping and the generic fallback); "none" and
    // "minimal" both reach this call as-is, since whether/how "off" is
    // spelled on the wire is itself backend-specific (Anthropic omits the
    // thinking block entirely; most others send reasoning_effort:"minimal").
    // May throw std::runtime_error for a known-but-unusable effort name
    // (Anthropic's "minimal" has no budget reading) -- the caller does not
    // catch it, so the error surfaces exactly as it did when this lived in
    // core.
    virtual std::pair<nlohmann::json, bool> DeriveParameters(
        const std::string& effort_word, std::optional<int> max_tokens) const = 0;
};

// Kind-string -> provider. Mirrors ModelCatalogRegistry: no closed enum, the
// first registration for a kind wins, and Register returns false on a
// collision so the loader can report which two plugins claimed it.
class VariantParamsRegistry {
public:
    // Returns false if `kind` was already registered or `provider` is null.
    bool Register(std::string kind, std::shared_ptr<VariantParamsProvider> provider);

    // --- Attribution, for collision reporting -- same seam as
    // ModelCatalogRegistry's; not thread-safe, registration is
    // single-threaded and finishes before any session exists. ------------
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Collision {
        std::string kind;
        std::string first_plugin;
        std::string second_plugin;
    };
    const std::vector<Collision>& collisions() const { return collisions_; }

    // nullptr when no plugin registered a mapping for this kind -- the
    // generic reasoning_effort fallback applies.
    VariantParamsProvider* Find(std::string_view kind) const;

    bool Has(std::string_view kind) const;

    // Sorted, for deterministic diagnostics and startup output.
    std::vector<std::string> kinds() const;

private:
    std::map<std::string, std::shared_ptr<VariantParamsProvider>, std::less<>> providers_;
    std::map<std::string, std::string, std::less<>> registrant_;  // kind -> plugin that holds it
    std::string attribution_;
    std::vector<Collision> collisions_;
};

}  // namespace minimal::llm
