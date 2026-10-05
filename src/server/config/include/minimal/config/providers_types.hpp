#pragma once

// The value types shared by ProvidersConfig and the discovery runtime state
// (CatalogRuntime): model references, variants, provider entries, resolved
// models, discovery outcomes and hooks. Split out of providers_config.hpp so the
// runtime state can live in its own header without a circular include.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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

// Immutable once parsed: written only at insertion (ProvidersConfig::Add /
// AddProvider, parse-time), never mutated in place, no lock needed. Ordered, so
// ModelRefs() is sorted without a second pass and two runs list providers
// identically.
struct ImmutableCatalog {
    std::map<std::string, std::shared_ptr<const ResolvedModel>> models_;  // "<provider>/<model>"
    std::map<std::string, ProviderEntry> providers_;                      // never mutated after parse
};

}  // namespace minimal::config
