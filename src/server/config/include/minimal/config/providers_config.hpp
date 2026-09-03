#pragma once

// providers.json: what backends exist, and which models each one serves.
//
// A PROVIDER IS A CONNECTION, NOT A MODEL, and that split is the whole point of
// this file's shape. The previous schema made an entry *be* a model -- name,
// base_url and model_name in one flat record -- which meant two models on one
// endpoint had to duplicate the connection, and `parameters`/`max_tokens` came
// from the entry no matter which model a preset had selected. One llama.cpp
// serving a 128k model and a 32k model was inexpressible: there was nowhere to
// put the second context window.
//
//   {
//     "providers": {
//       "local": {
//         "backend": "openai",
//         "url": "http://localhost:8000/v1",
//         "discover": true,                         <- list /models instead of naming every model
//         "max_tokens": 4096,                       <- default for every model here
//         "parameters": { "temperature": 0.3 },
//         "models": {                               <- optional when discover is true
//           "qwen4": { "context_window_tokens": 131072 },
//           "small": { "context_window_tokens": 32768, "max_tokens": 1024 }
//         }
//       }
//     }
//   }
//
// A MAP, not an array: the key IS the provider name, so it cannot disagree with
// a `name` field sitting beside it. Models nest the same way for the same reason.
//
// Addressed as `<provider>/<model>` -- "local/qwen4" -- everywhere a model is
// named: preset frontmatter, settings.default_model, /model, --model. One syntax,
// so a name that works in one place works in all of them.
//
// `backend` names a ProviderFactoryRegistry kind, looked up at session
// construction and not validated here: any plugin-registered kind is legal, so
// there is no closed enum to keep in sync.
//
// DISCOVERY (track N, docs/EXTENSIBILITY.md §2.6): `"discover": true` lets the
// entry's backend list its models at runtime instead of -- or in addition to --
// naming them statically. The fetch is LAZY: at first Resolve() that needs it,
// never at startup, through a hook injected after plugin load (the config layer
// never links plugins). Results live in a runtime cache and are NEVER written
// back to providers.json; the file remains the user's, read once. A failed
// discovery is negative-cached (not re-attempted per resolve) and degrades:
// static models still resolve, and a miss distinguishes "discovery succeeded,
// model not served" (a typo or a removed model) from "discovery failed" (a dead
// catalog) through discovery_status(). /providers refresh evicts the cache and
// refetches on demand.
//
// THREAD-SAFETY: Resolve()/ModelRefs()/discovery_status() are called from many
// session workers AND the read-only command pool (which runs beside live
// turns), so the lazy state is mutex-guarded. A returned ResolvedModel pointer
// borrows into node storage that is never mutated after insertion -- copy out
// what you need (ApplyModel does) rather than holding the pointer across calls.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/model_ref.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::config {

// Both live in common/, because the CLIENT names models too (--model) and must
// not accept something the server would reject. Re-exported here so server-side
// code reads in one namespace.
using common::ModelRef;
using common::ParseModelRef;
using common::SplitModelRef;

// One PROVIDER -- the connection an entry defines, kept even when it declares
// no static models (a discover-entry is addressable through its catalog).
// Provider-level values are the inheritance base for discovered models exactly
// as they are for static ones.
struct ProviderEntry {
    std::string name;
    std::string backend;           // ProviderFactoryRegistry / ModelCatalogRegistry kind
    llm::ProviderConfig connection;
    bool discover = false;
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;
    std::optional<std::int64_t> context_window_tokens;
};

// One model, with its provider's connection and its inherited settings already
// merged in. This is what a session needs to construct a provider and issue a
// request; nothing downstream has to know the file's shape.
struct ResolvedModel {
    // Its own name. Two providers may point at the same url and serve the same
    // model id -- one capped, one not -- so the connection and the model id
    // together do NOT identify which reference a session is running. Only this
    // does, and /providers needs to say which line is the active one.
    ModelRef ref;
    std::string backend;           // ProviderFactoryRegistry kind
    llm::ProviderConfig provider;  // url, api_key, model_name, timeouts
    // Provider-level values with the model's own layered on top, resolved at
    // LOAD (static models) or at DISCOVERY (discovered models) rather than per
    // lookup -- so the merged result is inspectable in one place and every
    // caller sees the same answer.
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;
    std::optional<std::int64_t> context_window_tokens;
};

// The result of one catalog consultation, as the injected hook reports it.
// The hook is built over the ModelCatalogRegistry in main.cpp; distinguishing
// "no catalog registered" from "catalog reachable but List failed" is what
// lets the miss reporting name a missing plugin instead of a vague network
// error.
struct DiscoveryOutcome {
    bool catalog_present = false;  // no ModelCatalog registered for the backend kind
    bool ok = false;               // List() succeeded
    std::string error;             // human-readable; set when !ok
    std::vector<llm::DiscoveredModel> models;
};

// Set once from main.cpp after plugin load; the config layer never links
// plugins, so discovery reaches it as a function. Called with NO ProvidersConfig
// lock held (only the per-provider fetch gate serializes concurrent catalog
// calls for one provider), so a slow catalog never blocks other config reads;
// it must not re-enter the same ProvidersConfig.
using DiscoveryHook = std::function<DiscoveryOutcome(const std::string& backend_kind,
                                                     const llm::ProviderConfig& connection)>;

// What the last discovery attempt for a provider concluded -- the input to
// miss reporting, so a typo and a dead catalog read differently.
struct DiscoveryStatus {
    bool attempted = false;  // discover-entry, hook consulted at least once
    bool ok = false;
    std::string error;  // set when attempted && !ok
};

// One provider refreshed by RefreshDiscovery().
struct DiscoveryRefreshOutcome {
    std::string provider;
    bool ok = false;
    std::string error;  // empty when ok
    std::size_t model_count = 0;
};

class ProvidersConfig {
public:
    // Movable so an owner can replace a config wholesale (the sanity check
    // loads its own copy). Not copyable -- the mutex. Moves happen only during
    // config load, before any thread can see either side; the mutexes are
    // locked anyway so the guarantee does not rest on that.
    ProvidersConfig(ProvidersConfig&& other) noexcept { *this = std::move(other); }
    ProvidersConfig& operator=(ProvidersConfig&& other) noexcept;
    ProvidersConfig(const ProvidersConfig&) = delete;
    ProvidersConfig& operator=(const ProvidersConfig&) = delete;
    ProvidersConfig() = default;
    // A snapshot of the resolved model -- null when the provider or the model
    // within it is unknown. Callers report; this layer does not know whether a
    // miss is a typo in a preset, in settings.json, or in something a user just
    // typed. The snapshot stays valid however long the holder keeps it, even
    // across a /providers refresh that replaces the entry.
    //
    // May trigger a discovery fetch on first need (lazy, once per provider per
    // process; a failure is negative-cached) -- so a "miss" can mean the model
    // is genuinely absent even under a discover-entry. See discovery_status().
    std::shared_ptr<const ResolvedModel> Resolve(const ModelRef& ref) const;

    bool empty() const { return catalog_.models_.empty(); }

    // Every `<provider>/<model>` this config can serve right now, sorted:
    // static entries plus DISCOVERED models already in the runtime cache. A
    // cold cache contributes none -- this is a listing, not a trigger to fetch.
    // What `/providers` lists and what a "no such model" error offers as
    // alternatives.
    std::vector<std::string> ModelRefs() const;

    // The providers this file defines, keyed by name -- including discover
    // entries that name no static model. Parse-time only, plus direct
    // construction in tests.
    const std::map<std::string, ProviderEntry>& provider_entries() const { return catalog_.providers_; }

    // Injection point for discovery (main.cpp, after plugin load). Once per
    // process; the hook is called under the config mutex and must not
    // re-enter this object.
    void SetDiscoveryHook(DiscoveryHook hook);

    // Whether discovery failed for a provider, and why. Untouched
    // (attempted == false) for a non-discover provider or a discover-entry the
    // session has not resolved against yet.
    DiscoveryStatus discovery_status(const std::string& provider_name) const;

    // Evicts every discover-entry's runtime cache and refetches it now, so the
    // result of /providers refresh is the fresh list, not a promise to fetch
    // later. Also drops discovered ResolvedModels belonging to the refreshed
    // providers. Returns one outcome per discover-entry (empty when the file
    // declares none). Const because it touches only mutex-guarded mutable
    // cache state. Each provider's fetch runs holding only that provider's
    // fetch gate -- never the global mutex -- so refreshing a slow catalog
    // does not stall other config readers, and providers refresh in parallel;
    // the per-provider gate serializes a refresh against a concurrent lazy
    // fetch of the same provider.
    std::vector<DiscoveryRefreshOutcome> RefreshDiscovery() const;

    // Insertion is parse-time only; exposed so ParseProvidersConfig can build one
    // without friending it, and so tests can construct a config directly.
    void Add(const ModelRef& ref, ResolvedModel resolved);
    void AddProvider(ProviderEntry entry);

private:
    // Immutable once parsed: written only at insertion (Add/AddProvider,
    // parse-time), never mutated in place, no lock needed. Ordered, so
    // ModelRefs() is sorted without a second pass and two runs list providers
    // identically.
    struct ImmutableCatalog {
        std::map<std::string, std::shared_ptr<const ResolvedModel>> models_;  // "<provider>/<model>"
        std::map<std::string, ProviderEntry> providers_;  // never mutated after parse
    };
    ImmutableCatalog catalog_;

    // --- lazy discovery state -------------------------------------------------
    // Everything the discovery cache needs, grouped so the split from
    // ImmutableCatalog above is structural, not just a naming convention.
    // Refresh replaces overlay_ entries wholesale -- old objects stay alive
    // through any shared_ptr still holding them -- so a Resolve snapshot
    // remains valid across refreshes.
    struct DiscoveryCacheState {
        // Serializes catalog fetches per provider. The network call runs
        // holding ONLY the provider's gate, so one hung catalog cannot stall
        // session creation or listings -- the failure mode a global lock
        // across the fetch would produce. Lock order is always gate ->
        // mutex_, never the reverse.
        struct FetchGate {
            std::mutex mtx;
        };
        // Entries are created lazily (Resolve) or up front (RefreshDiscovery)
        // and never erased, so a gate reference held across an unlock stays
        // valid.
        mutable std::map<std::string, FetchGate> gates_;  // by provider name

        struct DiscoveryCache {
            std::uint64_t generation = 0;  // future config reload; always 0 today
            bool attempted = false;
            bool ok = false;
            std::string error;
            std::map<std::string, llm::DiscoveredModel> models;  // by model id
        };

        // Guards gates_, discovery_, overlay_, hook_ and generation_.
        mutable std::mutex mutex_;
        DiscoveryHook hook_;
        std::uint64_t generation_ = 0;
        mutable std::map<std::string, DiscoveryCache> discovery_;  // by provider name
        // Discovered models, and overlay copies of static models enriched with
        // a discovered context window. Keyed "<provider>/<model>".
        mutable std::map<std::string, std::shared_ptr<const ResolvedModel>> overlay_;
    };
    DiscoveryCacheState cache_;

    const ProviderEntry* FindProviderLocked(const std::string& name) const;
    // Calls the hook -- the network I/O. Requires NO lock held; `entry` points
    // into catalog_.providers_, which is parse-time only and never mutated after.
    DiscoveryOutcome FetchOutcome(const ProviderEntry& entry) const;
    // Writes one outcome into the cache and materializes every discovered
    // model into the overlay, so a later Resolve of any of them is a lookup,
    // never a rebuild. cache_.mutex_ held.
    void PublishLocked(const ProviderEntry& entry, const DiscoveryOutcome& outcome,
                       DiscoveryCacheState::DiscoveryCache& cache) const;
    // The discover-entry half of Resolve: ensures the provider's catalog has
    // been consulted this generation (fetching under the per-provider gate
    // with no config lock held, double-checking after the wait), then returns
    // the model -- from the overlay when it is already there, else built from
    // the entry and the discovery cache. Null on a dead catalog or a model it
    // does not serve. `lock` holds mutex_ on entry and on every return.
    std::shared_ptr<const ResolvedModel> ResolveDiscovered(const ProviderEntry& entry,
                                                           const std::string& model_id,
                                                           const std::string& ref_key,
                                                           std::unique_lock<std::mutex>& lock) const;
};

// What providers.json actually offers, for a "no such model" error. A miss is
// almost always a typo or a provider the user has not configured yet, and
// both are answered by seeing the list rather than by being told the name was
// wrong.
std::string DescribeModels(const ProvidersConfig& providers);

// Miss reporting: a typo and a dead catalog must read differently. Silent
// when discovery was never attempted or succeeded -- the latter means the
// model is genuinely not served, which DescribeModels already says.
std::string DescribeDiscovery(const ProvidersConfig& providers, const std::string& provider_name);

// Pure. Throws std::runtime_error on: a top-level array (the pre-`models` schema
// -- refused by name rather than silently misparsed), a provider missing
// `backend`/`url`, a provider with no `models` UNLESS it sets "discover": true
// (a discover-entry is addressable through its catalog), both api_key and
// api_key_env set, or api_key_env naming an unset variable. Neither key set is
// NOT an error -- local servers routinely need none.
ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or is not
// valid JSON, or per ParseProvidersConfig above.
ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path);

}  // namespace minimal::config
