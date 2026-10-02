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

#include "minimal/config/catalog_runtime.hpp"
#include "minimal/config/providers_types.hpp"

namespace minimal::config {

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
    // static entries plus DISCOVERED models and their variant siblings already
    // in the runtime cache. A cold cache contributes none -- this is a
    // listing, not a trigger to fetch. What `/providers` lists and what a
    // "no such model" error offers as alternatives.
    std::vector<std::string> ModelRefs() const;

    // The providers this file defines, keyed by name -- including discover
    // entries that name no static model. Parse-time only, plus direct
    // construction in tests.
    const std::map<std::string, ProviderEntry>& provider_entries() const { return catalog_.providers_; }

    // Injection point for discovery (main.cpp, after plugin load). The hook
    // is called under the config mutex and must not re-enter this object.
    // Setting it invalidates every discovery attempted so far (a lookup made
    // before the hook existed was negative-cached), so each provider refetches
    // at its next resolve.
    void SetDiscoveryHook(DiscoveryHook hook);

    // How long a FAILED discovery stays negative-cached before the next
    // resolve of that provider retries it. A backend that was down (or simply
    // started after the server) used to stay "unknown model" until a manual
    // ctrl+r; a success is kept until a refresh. Default 30 s. A retry runs
    // under the per-provider gate like any fetch, so a dead catalog costs one
    // transport timeout per interval, not one per resolve.
    void SetDiscoveryRetryBackoff(std::chrono::milliseconds backoff);

    // Injects the metadata-enrichment hook (docs/EXTENSIBILITY.md §2.13).
    // Consulted lazily: when a static model lacking a window/cap resolves, or
    // a discover-entry's listing leaves fields unset -- never at startup, and
    // never for a model that already has the fields declared.
    void SetMetadataHook(MetadataHook hook);

    // Injection point for the DISCOVERED-variant path (ResolveVariant),
    // mirroring SetDiscoveryHook/SetMetadataHook -- consulted lazily, after
    // this object already exists, so a post-construction setter works fine
    // here (unlike the static list-form path, which needs the hook DURING
    // ParseProvidersConfig -- see VariantParametersFor's own threading).
    // Once per process; called under the config mutex, must not re-enter.
    void SetVariantParamsHook(VariantParamsHook hook);

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

    // The variant NAMES declared for `ref`'s base model -- from the model's own
    // entry recipe (static) or the provider-level recipe filtered by what the
    // catalog advertised (discovered; cold cache contributes nothing, the same
    // rule ModelRefs applies). The base ref itself is NOT included: the base is
    // always addressable, and callers that want "base plus variants" concatenate.
    // Read-only, mutex-guarded, safe from the command pool; never triggers a
    // discovery fetch.
    std::vector<std::string> VariantsOf(const ModelRef& ref) const;

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

    CatalogRuntime runtime_;

    const ProviderEntry* FindProviderLocked(const std::string& name) const;
    // The body of VariantsOf, minus the ref parse -- shared with ModelRefs'
    // variant-refs union, which needs the same precedence (static-catalog
    // siblings first, the discovery cache's autodetected ladder only where the
    // base has none and did not opt out) while already holding runtime_.mutex_.
    // runtime_.mutex_ held.
    std::vector<std::string> VariantsOfLocked(const std::string& provider_name,
                                              const std::string& base_id) const;
    // Calls the hook -- the network I/O. Requires NO lock held; `entry` points
    // into catalog_.providers_, which is parse-time only and never mutated after.
    DiscoveryOutcome FetchOutcome(const ProviderEntry& entry) const;
    // Writes one outcome into the cache and materializes every discovered
    // model into the overlay, so a later Resolve of any of them is a lookup,
    // never a rebuild. runtime_.mutex_ held.
    void PublishLocked(const ProviderEntry& entry, const DiscoveryOutcome& outcome,
                       CatalogRuntime::DiscoveryCache& cache) const;
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
    // Resolves `<base>@<variant>` -- strips the suffix, resolves the base (any
    // path: static, enriched, or discovered), rebuilds the sibling via
    // MakeVariantResolved, and serves it from overlay_ so the rebuild happens
    // at most once per ref per process. `lock` holds mutex_ on entry; released
    // only inside ResolveDiscovered's fetch, as today. Null when the base is
    // unknown or the variant name was never declared for it (an unknown variant
    // is a MISS -- the caller reports "unknown model", the honest outcome for a
    // typo -- never a synthesized guess).
    std::shared_ptr<const ResolvedModel> ResolveVariant(const ProviderEntry& entry,
                                                        const std::string& base_id,
                                                        const std::string& variant_name,
                                                        const std::string& ref_key,
                                                        std::unique_lock<std::mutex>& lock) const;
    // The metadata-enrichment half of a resolve: the static model at `ref_key`
    // is missing window/cap (or has no autodetected ladder recorded), the
    // entry has not declined enrichment, and a metadata hook is injected.
    // Unlocks, consults the hook under the per-provider gate (the same
    // discipline ResolveDiscovered uses -- a slow catalog must not stall
    // other config reads), re-locks, and writes the gap-filled overlay copy
    // plus the autodetected variant recipe into metadata_variants_.
    // Cache_.mutex_ held on entry and on return; dropped across the hook.
    void EnrichFromMetadata(const ProviderEntry& entry, const std::string& model_id,
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

// Pure (given `variant_params_hook`). Throws std::runtime_error on: a
// top-level array (the pre-`models` schema -- refused by name rather than
// silently misparsed), a provider missing `backend`/`url`, a provider with no
// `models` UNLESS it sets "discover": true (a discover-entry is addressable
// through its catalog), both api_key and api_key_env set, or api_key_env
// naming an unset variable. Neither key set is NOT an error -- local servers
// routinely need none.
//
// `variant_params_hook` derives wire parameters for a static
// `"variants": [<effort names>]` list-form entry -- see VariantParamsHook.
// Defaulted to {} so a caller with no plugin registry handy (most tests, and
// any caller that only wants the generic reasoning_effort shape) keeps
// compiling.
ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc,
                                     const VariantParamsHook& variant_params_hook = {});

// Thin: reads the file and parses it. Throws if the file is missing or is not
// valid JSON, or per ParseProvidersConfig above.
ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path,
                                    const VariantParamsHook& variant_params_hook = {});

}  // namespace minimal::config
