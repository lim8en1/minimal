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

#include <chrono>
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
#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/variant_params.hpp"

namespace minimal::config {

// Both live in common/, because the CLIENT names models too (--model) and must
// not accept something the server would reject. Re-exported here so server-side
// code reads in one namespace.
using common::ModelRef;
using common::ParseModelRef;
using common::SplitModelRef;

// One effort VARIANT of a base model: `<provider>/<model>@<name>` resolves to a
// sibling ResolvedModel -- same connection, same WIRE model id (the `@name`
// suffix is addressing only and never sent -- agent_loop sends
// provider.model_name verbatim), with the variant's `parameters` overlaid on the
// base's and optional variant-scoped output cap / context window.
//
// Two config spellings share one `variants` key (the allowed_skills
// list-or-map convention; JSON's array/object distinction makes the forms
// unambiguous):
//
//   "variants": ["low", "medium", "xhigh"]     -- effort names; the parameters
//                                                 are derived per backend kind
//                                                 (see VariantParametersFor)
//   "variants": {                              -- full opencode-style bodies
//     "deep": { "parameters": { "reasoning": {"effort": "high"} },
//               "max_tokens": 16384 }
//   }
//
// A variant body's known keys: parameters, max_tokens, context_window_tokens;
// anything else is a named parse error. Effort names outside the known
// vocabulary are refused where the mistake is, for the same reason InheritNumber
// refuses 4096.7.
struct ModelVariant {
    std::string name;             // the `@name` suffix, e.g. "high"
    nlohmann::json parameters = nlohmann::json::object();  // overlaid ONTO the base's
    std::optional<int> max_tokens;
    std::optional<std::int64_t> context_window_tokens;
};

// Kind-keyed provider-metadata lookup (ARCHITECTURE.md, "Core vs. plugin
// surface"), injected the same way DiscoveryHook/MetadataHook are: main.cpp
// builds it over a VariantParamsRegistry once, after plugin load; the config
// layer never links plugins. UNLIKE those two, this is threaded as a
// PARSE-TIME ARGUMENT rather than a post-construction setter: the static
// `"variants": [...]` list form derives its wire parameters DURING
// ParseProvidersConfig itself (MakeVariantResolved, called while the config
// is still being built), before any ProvidersConfig object exists to call a
// setter on. `effort_word` arrives normalized (see VariantParametersFor).
// nullopt return means "no plugin-registered mapping for this kind" -- the
// generic reasoning_effort fallback applies, same as any backend that never
// had one.
using VariantParamsHook = std::function<std::optional<std::pair<nlohmann::json, bool>>(
    const std::string& backend_kind, const std::string& effort_word, std::optional<int> max_tokens)>;

// Translates a registry pointer into a VariantParamsHook -- the Find-then-
// invoke glue shared by every call site that owns a VariantParamsRegistry
// (main.cpp's own hook for the two eager parse call sites, sanity_check.cpp,
// AgentPresetRegistry's constructor). Null registry -> an always-nullopt
// hook, so the generic fallback applies uniformly whether the registry is
// absent (a test with no plugin registry) or simply has no mapping for a
// particular kind.
VariantParamsHook MakeVariantParamsHook(const llm::VariantParamsRegistry* registry);

// The effort vocabulary and the derived parameters, per backend kind. Pure
// (given `hook`); shared by the static parse, discovery synthesis, and (via
// the same constants) the variant-name validation.
//
// A THIN DISPATCHER, not the backend-specific knowledge itself (relocated to
// plugins/server/provider-anthropic -- see ARCHITECTURE.md, "Core vs. plugin
// surface", and llm/variant_params.hpp): "xhigh"/"max" fold to "high" once,
// here, so the fold cannot drift between a plugin's mapping and the fallback
// below; `hook` is then consulted for `backend` (see VariantParamsHook) and,
// on a hit, owns the rest of the derivation entirely, including how "none"
// spells "off" on that backend's wire. On a miss -- no hook, or the kind has
// no registered provider -- every backend gets the same generic
// `reasoning_effort: "<name>"` shape ("none" spells "minimal"): the
// OpenAI-compatible-proxy vocabulary is the closest thing to a lingua franca,
// and a backend that rejects it fails the turn with the API's own message
// rather than silently doing nothing.
//
// `max_tokens` is the base model's resolved output cap (nullopt = unknown),
// forwarded to the hook for backends whose wire shape needs it (Anthropic's
// budget clamp). The second element of the returned pair is false when a
// value was clamped, so the caller can warn.
std::pair<nlohmann::json, bool> VariantParametersFor(const std::string& backend,
                                                     const std::string& effort,
                                                     std::optional<int> max_tokens,
                                                     const VariantParamsHook& hook = {});

// The known effort names, for validation and error messages. Sorted.
const std::vector<std::string>& KnownEffortNames();

// True when `name` is a known effort name (the R2 list form is validated
// against this; the R3 map form's names are free-form).
bool IsKnownEffortName(const std::string& name);

// Splits a model id at the LAST '@': "x@high" -> ("x", "high"), "x" -> ("x", "").
// The model half may itself contain '/' (OpenRouter ids); '@' appears in no live
// catalog id (checked against OpenRouter's 443-entry catalog, 2025-09), and a
// tail that matches no declared variant falls back to treating the whole half
// as the id (see Resolve) -- so this split is always safe to attempt.
std::pair<std::string, std::string> SplitVariantSuffix(const std::string& model_id);
// One PROVIDER -- the connection an entry defines, kept even when it declares
// no static models (a discover-entry is addressable through its catalog).
// Provider-level values are the inheritance base for discovered models exactly
// as they are for static ones.
struct ProviderEntry {
    std::string name;
    std::string backend;           // ProviderFactoryRegistry / ModelCatalogRegistry kind
    llm::ProviderConfig connection;
    // How connection.api_key came to be. Names the SOURCE -- "inline api_key",
    // "api_key_env '<NAME>'", "auth-file (<path>)", "auth-file (no key found)"
    // -- never the key itself, so it is safe to log verbatim. Empty when the
    // entry configured no credential at all (a keyless local server is normal).
    std::string api_key_origin;
    bool discover = false;
    // Metadata enrichment knobs (docs/EXTENSIBILITY.md §2.13). `metadata`
    // false declines enrichment for this entry (the auto default is on);
    // `metadata_match` selects the id-matching policy for the entry
    // (default: the config-wide "full"); `metadata_provider` pins the
    // catalog's provider-table choice (catalog-specific spelling, passed
    // through verbatim -- the escape hatch when base_url matching picks the
    // wrong table).
    bool metadata = true;
    llm::MetadataMatch metadata_match = llm::MetadataMatch::Full;
    std::string metadata_provider;
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;
    std::optional<std::int64_t> context_window_tokens;
    // The entry's `variants` recipe, in either spelling (see ModelVariant):
    // list form = effort names (validated against KnownEffortNames at parse),
    // map form = named full bodies. On the PROVIDER these apply to every model
    // under the entry -- including discovered ones, where they double as the
    // autodiscovery recipe (an entry with no variants of its own gets the
    // model's own advertised effort ladder (autodetected) for models whose
    // efforts). A model's own `variants` replaces the provider's wholesale, the
    // same per-model-beats-per-provider precedence InheritParameters applies.
    std::vector<ModelVariant> variants;
    // Model-picker filters (opencode compat). whitelist keeps ONLY the listed
    // model ids; blacklist removes the listed ones from whatever remains.
    // Resolution is unaffected: a blacklisted model is still addressable by
    // an explicit <provider>/<model>, it just does not appear in the picker.
    std::vector<std::string> whitelist;
    std::vector<std::string> blacklist;
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
    // True when the model's own entry or its provider's declared a `variants`
    // key at all -- including an explicit empty array/object, which means
    // "no effort variants for this model" as an operator's deliberate choice.
    // VariantsOf consults this to distinguish that opt-out from "nothing
    // declared, defer to the discovery-cache's autodetected ladder": both
    // shapes produce an empty ParseVariants() result, so the vector alone
    // cannot tell them apart.
    bool variants_declared = false;
};

// The static entry's resolved model with its discovery twin's window / output
// cap filling only the gaps the static file left open -- each field
// independently, since a model with a window but no cap (or the reverse)
// reaches this too. The static value ALWAYS wins where it is present: the
// operator typed it. Shared by ResolveDiscovered (which publishes the enriched
// base) and ResolveVariant (whose sibling must carry the same window a session
// on the base would get -- otherwise a /think switch silently drops
// compaction). Free function: the enrichment rule must not drift between the
// two call sites.
ResolvedModel EnrichFromDiscovery(const ResolvedModel& static_model,
                                  const llm::DiscoveredModel& discovered);

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

// The metadata-enrichment hook (docs/EXTENSIBILITY.md §2.13), injected the
// same way as DiscoveryHook: main.cpp builds it over the
// MetadataCatalogRegistry; the config layer never links plugins. The query
// carries the backend kind, the model id (already addressable -- enrichment
// never invents one), the entry's base_url and name, and the entry's
// `metadata_provider` pin verbatim. nullopt = "no answer" -- never an error;
// failures are the hook implementation's to degrade. Called with no config
// lock held, like the discovery hook.
using MetadataHook =
    std::function<std::optional<llm::DiscoveredModel>(const llm::MetadataQuery& query,
                                                      llm::MetadataMatch match)>;

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
            // When the last attempt finished; a failed one is retried once
            // discovery_retry_backoff_ has elapsed.
            std::chrono::steady_clock::time_point attempted_at;
            bool attempted = false;
            bool ok = false;
            std::string error;
            std::map<std::string, llm::DiscoveredModel> models;  // by model id
            // The variant recipe applied per discovered model, computed ONCE at
            // publish (the entry recipe, or the model's autodetected ladder
            // the model's catalog-advertised efforts) -- ResolveVariant looks
            // the declared variant up here instead of re-deriving the
            // recipe-precedence rules a second time beside the publish path.
            std::map<std::string, std::vector<ModelVariant>> variants_by_model;
        };

        // Guards gates_, discovery_, overlay_, hook_, metadata_hook_,
        // variant_params_hook_, metadata_variants_ and generation_.
        mutable std::mutex mutex_;
        DiscoveryHook hook_;
        MetadataHook metadata_hook_;
        // Consulted only by the discovered-variant path (ResolveVariant); the
        // static list-form path reads its hook from a function parameter
        // instead (see VariantParametersFor/ParseProvidersConfig).
        VariantParamsHook variant_params_hook_;
        std::uint64_t generation_ = 0;
        std::chrono::milliseconds discovery_retry_backoff_{30000};
        mutable std::map<std::string, DiscoveryCache> discovery_;  // by provider name
        // Discovered models, and overlay copies of static models enriched with
        // a discovered context window. Keyed "<provider>/<model>".
        mutable std::map<std::string, std::shared_ptr<const ResolvedModel>> overlay_;
        // The variant recipes metadata enrichment contributed, per
        // "<provider>/<model>" base key -- the static-side twin of
        // DiscoveryCache::variants_by_model. Written by the lazy metadata
        // enrichment in Resolve/ResolveVariant, read by VariantsOfLocked and
        // ResolveVariant. Precedence makes this safe to fill eagerly or
        // lazily: a parse-time declared sibling lives in the catalog and wins
        // regardless of what lands here, and `variants_declared` bases never
        // consult it. An entry in metadata_tombstones_ has already consulted
        // the hook and gotten "no answer" -- recorded so a miss costs one
        // Lookup, not one per resolve.
        mutable std::map<std::string, std::vector<ModelVariant>> metadata_variants_;
        mutable std::map<std::string, bool> metadata_tombstones_;  // ref key -> miss recorded
    };
    DiscoveryCacheState cache_;

    const ProviderEntry* FindProviderLocked(const std::string& name) const;
    // The body of VariantsOf, minus the ref parse -- shared with ModelRefs'
    // variant-refs union, which needs the same precedence (static-catalog
    // siblings first, the discovery cache's autodetected ladder only where the
    // base has none and did not opt out) while already holding cache_.mutex_.
    // cache_.mutex_ held.
    std::vector<std::string> VariantsOfLocked(const std::string& provider_name,
                                              const std::string& base_id) const;
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
