#pragma once

// Model auto-discovery (track N, docs/EXTENSIBILITY.md §2.6).
//
// A ModelCatalog lists the models an OpenAI-shaped backend serves, so a
// providers.json entry does not have to name every `<model>` by hand -- and so
// `context_window_tokens`, the field window-relative compaction resolves
// against, stops being the one most likely to be stale or absent. One
// implementation beside provider-openai covers llama.cpp, vLLM and OpenRouter
// alike: a GET on the same base URL, with per-server field names.
//
// A SECOND REGISTRY, not a capability query on LlmProvider: that interface is
// deliberately two methods, and ARCHITECTURE.md deleted an earlier
// capability-query layer for having no users. The catalog is keyed by the SAME
// kind string the provider factory registry uses, so a providers.json entry's
// `backend` selects both how to chat and how to list.
//
// Discovery results are RUNTIME ONLY: nothing is ever written back to
// providers.json (config-read-once stands), and the fetch is lazy -- at first
// resolve, never at startup. Failure degrades: static models still resolve.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/llm/provider.hpp"

namespace minimal::llm {

// One model as the backend reports it. `id` is the <model> half of
// <provider>/<model>. `context_window_tokens` feeds compaction and the status
// surface. `metadata` (pricing, quantization, ...) is opaque to core.
// `max_tokens` is the backend's own output cap when it reports one
// (OpenRouter's `top_provider.max_completion_tokens`); LAST so existing
// brace-initializers that stop at `metadata` keep compiling.
struct DiscoveredModel {
    std::string id;
    std::optional<std::int64_t> context_window_tokens;
    nlohmann::json metadata = nlohmann::json::object();
    std::optional<int> max_tokens;
};

// The effort ladder a catalog entry advertises, from its metadata's
// `reasoning` object -- OpenRouter's live /models spelling (e.g.
// {"mandatory": true, "supported_efforts": ["max","high","low"]}), which
// ParseModelList stores verbatim in DiscoveredModel::metadata. The observed
// shapes: supported_efforts present (a list of effort words), or only
// mandatory/default_enabled (a reasoning toggle, no ladder), or nothing
// reasoning-shaped at all. Returns the advertised effort words (unvalidated;
// callers intersect with the known vocabulary); empty = no ladder, which is
// also the answer for a toggle-only model. Tolerates every malformed shape by
// degrading to empty -- the MaxCompletionTokens posture: a hostile or foreign
// metadata body yields "no variants", never a guess.
//
// models.dev's normalized `reasoning_options` spelling
// ([{"type":"effort","values":[...]}]) is ALSO read, so a Models.dev-style
// catalog plugin (docs/EXTENSIBILITY.md deferred table) feeds this unchanged.
std::vector<std::string> AdvertisedEfforts(const nlohmann::json& metadata);

class ModelCatalog {
public:
    virtual ~ModelCatalog() = default;

    // Lists every model the backend behind `config` currently serves. Const
    // because implementations hold no mutable state across calls -- the same
    // contract as LlmProvider, which is what makes sharing one instance safe
    // across resolvers on session workers and the command pool.
    //
    // Throws on transport failure or a non-2xx response; the caller (the
    // discovery cache) negative-caches the failure rather than retrying per
    // resolve. `config.model_name` is unused here -- a catalog lists, it does
    // not chat.
    virtual std::vector<DiscoveredModel> List(const ProviderConfig& config) const = 0;
};

// Kind-string -> catalog. Mirrors ProviderFactoryRegistry: no closed enum, the
// first registration for a kind wins, and Register returns false on a
// collision so the loader can report which two plugins claimed it.
class ModelCatalogRegistry {
public:
    // Returns false if `kind` was already registered or `catalog` is null --
    // the caller decides whether that is fatal, since the loader wants to name
    // both claimants rather than throw from inside a plugin's callback.
    bool Register(std::string kind, std::shared_ptr<ModelCatalog> catalog);

    // --- Attribution, for collision reporting -------------------------------
    //
    // The same scoped window ProviderFactoryRegistry (and ToolRegistry/
    // CommandRegistry) use, for the same reason: Register() has no idea who
    // is calling, and a collision has to be able to name BOTH plugins. Not
    // thread-safe and does not need to be -- plugin loading is
    // single-threaded and finishes before any session exists.
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Collision {
        std::string kind;
        std::string first_plugin;   // the registration that holds the slot
        std::string second_plugin;  // the one that was dropped
    };
    const std::vector<Collision>& collisions() const { return collisions_; }

    // nullptr when no plugin registered a catalog for this kind. A providers.json
    // entry with "discover": true whose backend has no catalog is a startup
    // sanity-check warning, not a crash: the entry may still have static models.
    ModelCatalog* Find(std::string_view kind) const;

    bool Has(std::string_view kind) const;

    // Sorted, for deterministic diagnostics and startup output.
    std::vector<std::string> kinds() const;

private:
    std::map<std::string, std::shared_ptr<ModelCatalog>, std::less<>> catalogs_;
    std::map<std::string, std::string, std::less<>> registrant_;  // kind -> plugin that holds it
    std::string attribution_;
    std::vector<Collision> collisions_;
};

}  // namespace minimal::llm
