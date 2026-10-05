#include "minimal/config/model_catalog_service.hpp"

#include "minimal/config/providers_internal.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/json_config_loader.hpp"

namespace minimal::config {

std::vector<std::string> ModelCatalogService::VariantsOf(const ModelRef& ref) const {
    // A ref that already names a variant answers with nothing -- the base is
    // the thing that carries a variant list; `@x@y` is not a shape anyone can
    // have declared.
    const auto [base_id, variant_name] = SplitVariantSuffix(ref.model);
    if (!variant_name.empty()) return {};
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    return VariantsOfLocked(ref.provider, base_id);
}

// The variant recipe lookup under runtime_.mutex_ -- the body of VariantsOf,
// shared with ModelRefs' variant-refs union (which needs the same
// static-catalog-beats-discovery-cache precedence while already holding the
// mutex). See VariantsOf's public contract for the rules.
std::vector<std::string> ModelCatalogService::VariantsOfLocked(const std::string& provider_name,
                                                           const std::string& base_id) const {
    std::vector<std::string> out;
    const std::string key = provider_name + "/" + base_id;

    // Static base: parse-time synthesis put every declared sibling in the
    // catalog; list the refs that share the base prefix. A static base that
    // declared its own variants answers from here alone -- explicit beats
    // autodetection. But a static base that declared NONE on a discover entry
    // (the common shape: a static twin supplying parameters the discovery
    // listing cannot express) still carries the autodetected ladder, so an
    // empty answer falls through to the discovery-cache lookup below.
    const std::string prefix = key + "@";
    if (const auto base_it = catalog_.models_.find(key); base_it != catalog_.models_.end()) {
        for (const auto& [candidate, unused] : catalog_.models_) {
            (void)unused;
            if (candidate.rfind(prefix, 0) == 0) out.push_back(candidate.substr(prefix.size()));
        }
        if (!out.empty()) return out;
        // An explicit `variants: []` (or `{}`) is an operator's deliberate
        // opt-out, not "nothing to say" -- honor it here rather than falling
        // through to the discovery cache's autodetected ladder below, which
        // would silently re-enable the variants the entry tried to suppress.
        if (base_it->second->variants_declared) return out;
    }
    // Discovered base (cold cache contributes nothing -- a listing, never a
    // fetch trigger, the same rule ModelRefs applies), or a static base whose
    // discovery twin advertises a ladder: the recipe recorded at publish.
    // NB: cold-cache-here does NOT consult metadata either -- VariantsOf is a
    // listing, and triggering enrichment I/O from a listing would make the
    // picker latency-bound. The ladder lands here the first time the base (or
    // one of its variants) actually resolves.
    if (const auto record = runtime_.discovery_record(provider_name); record && record->ok) {
        const auto variants_it = record->variants_by_model.find(base_id);
        if (variants_it != record->variants_by_model.end()) {
            for (const auto& variant : variants_it->second) out.push_back(variant.name);
            return out;
        }
        // Discovery is ok but this base is a static-only model its listing
        // does not name: fall through -- the metadata recipe (if the base was
        // enriched) still applies.
    }
    // The metadata-enrichment recipe, when the base has been enriched
    // already. Not a consultation trigger -- see the NB above.
    if (const auto metadata_it = runtime_.metadata_variants_.find(key);
        metadata_it != runtime_.metadata_variants_.end()) {
        for (const auto& variant : metadata_it->second) out.push_back(variant.name);
    }
    return out;
}

const ProviderEntry* ModelCatalogService::FindProviderLocked(const std::string& name) const {
    const auto it = catalog_.providers_.find(name);
    return it == catalog_.providers_.end() ? nullptr : &it->second;
}

void ModelCatalogService::SetDiscoveryHook(DiscoveryHook hook) {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    runtime_.hook_ = std::move(hook);
    // A discovery attempted under the previous hook -- typically none at all,
    // when the startup sanity check resolved against this catalog before
    // plugins' catalogs were wired in -- is stale: refetch it at next resolve
    // rather than serving its negative-cached "no discovery hook set".
    ++runtime_.generation_;
}

void ModelCatalogService::SetDiscoveryRetryBackoff(std::chrono::milliseconds backoff) {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    runtime_.discovery_retry_backoff_ = backoff;
}

void ModelCatalogService::SetMetadataHook(MetadataHook hook) {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    runtime_.metadata_hook_ = std::move(hook);
}

void ModelCatalogService::SetVariantParamsHook(VariantParamsHook hook) {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    runtime_.variant_params_hook_ = std::move(hook);
}

// One Resolve call's working state. Owns the unique_lock on runtime_.mutex_ for
// the call; the only places it is released are Unlocked scopes, which re-lock on
// every exit path (a throwing hook included), so "the config lock is held
// unless this block says otherwise" is something the compiler checks rather than
// a convention each function repeats.
class ModelCatalogService::Resolution {
public:
    explicit Resolution(const ModelCatalogService& service)
        : svc_(service),
          catalog_(service.catalog_),
          runtime_(service.runtime_),
          lock_(service.runtime_.mutex_) {}

    std::shared_ptr<const ResolvedModel> Resolve(const ModelRef& ref);

private:
    // Releases the config lock for a scope (hook I/O, waiting on a gate).
    class Unlocked {
    public:
        explicit Unlocked(std::unique_lock<std::mutex>& lock) : lock_(lock) { lock_.unlock(); }
        ~Unlocked() { lock_.lock(); }
        Unlocked(const Unlocked&) = delete;
        Unlocked& operator=(const Unlocked&) = delete;

    private:
        std::unique_lock<std::mutex>& lock_;
    };

    // Takes the provider's fetch gate, returning with BOTH the gate held and the
    // config lock re-held. Lock order is always gate -> runtime_.mutex_, never
    // the reverse (runtime_.gates_ is only ever touched through gate_for, under
    // mutex_, or locked before it) -- and this is the one place it is spelled.
    std::unique_lock<std::mutex> AcquireGate(const std::string& provider) {
        std::unique_lock<std::mutex> gate;
        const Unlocked released(lock_);
        gate = std::unique_lock<std::mutex>(runtime_.gate_for(provider).mtx);
        return gate;
    }

    // The discover-entry half of Resolve: ensures the provider's catalog has
    // been consulted this generation (fetching under the per-provider gate
    // with no config lock held, double-checking after the wait), then returns
    // the model -- from the overlay when it is already there, else built from
    // the entry and the discovery cache. Null on a dead catalog or a model it
    // does not serve.
    std::shared_ptr<const ResolvedModel> ResolveDiscovered(const ProviderEntry& entry,
                                                           const std::string& model_id,
                                                           const std::string& ref_key);
    // Resolves `<base>@<variant>` -- strips the suffix, resolves the base (any
    // path: static, enriched, or discovered), rebuilds the sibling via
    // MakeVariantResolved, and serves it from overlay_ so the rebuild happens
    // at most once per ref per process. Null when the base is unknown or the
    // variant name was never declared for it (an unknown variant is a MISS --
    // the caller reports "unknown model", never a synthesized guess).
    std::shared_ptr<const ResolvedModel> ResolveVariant(const ProviderEntry& entry,
                                                        const std::string& base_id,
                                                        const std::string& variant_name,
                                                        const std::string& ref_key);
    // The metadata-enrichment half of a resolve: the static model at `ref_key`
    // is missing window/cap (or has no autodetected ladder recorded), the
    // entry has not declined enrichment, and a metadata hook is injected.
    // Consults the hook under the per-provider gate with the config lock
    // released, then writes the gap-filled overlay copy plus the autodetected
    // variant recipe into metadata_variants_.
    void EnrichFromMetadata(const ProviderEntry& entry, const std::string& model_id,
                            const std::string& ref_key);

    const ModelCatalogService& svc_;
    const ImmutableCatalog& catalog_;
    const CatalogRuntime& runtime_;
    std::unique_lock<std::mutex> lock_;
};

std::shared_ptr<const ResolvedModel> ModelCatalogService::Resolve(const ModelRef& ref) const {
    Resolution resolution(*this);
    return resolution.Resolve(ref);
}

std::shared_ptr<const ResolvedModel> ModelCatalogService::Resolution::Resolve(const ModelRef& ref) {
    const std::string key = ref.str();

    // A `@variant` suffix is checked FIRST: the ref does not exist as a static
    // or discovered id, and the suffix must be validated against the base
    // model's declared variants, not treated as a (missing) raw id.
    if (const auto [base_id, variant_name] = SplitVariantSuffix(ref.model);
        !variant_name.empty()) {
        const std::string base_key = ref.provider + "/" + base_id;
        // The base may be a static entry or a discovered one; Resolve of the
        // stripped ref answers that (and triggers the lazy fetch, which is
        // what makes a discovered base's variants resolvable on first use).
        if (const ProviderEntry* entry = svc_.FindProviderLocked(ref.provider);
            entry != nullptr) {
            if (auto resolved = ResolveVariant(*entry, base_id, variant_name, key)) {
                return resolved;
            }
        }
        // Fall through to the plain-id paths ONLY when the base id itself
        // contains an '@' that SplitVariantSuffix would have misread. Re-split
        // is unnecessary: if the base id exists as a literal (no live catalog
        // serves '@', but a hand-written one may), honor it verbatim.
        if (catalog_.models_.count(key) != 0) {
            return catalog_.models_.at(key);
        }
        // Not a declared variant, not a literal '@' id: a miss, reported by
        // the caller as "unknown model" -- never a synthesized guess.
        return nullptr;
    }

    if (const auto it = catalog_.models_.find(key); it != catalog_.models_.end()) {
        if (it->second->context_window_tokens && it->second->max_tokens) return it->second;
        // A static entry usually resolves as parsed. Under a discover-entry, a
        // model whose context_window_tokens or max_tokens is absent can still
        // be enriched from the catalog -- but only by handing back an overlay
        // COPY: the parsed node must never be mutated after a snapshot of it
        // has escaped.
        if (const auto enriched = runtime_.overlay_.find(key); enriched != runtime_.overlay_.end()) {
            return enriched->second;
        }
        if (const ProviderEntry* entry = svc_.FindProviderLocked(ref.provider);
             entry != nullptr && entry->discover) {
            // The catalog's answer for a model it lists, or nothing when the
            // fetch failed. A STATIC hit degrades to its parsed self either
            // way -- a dead backend must not take a hand-written model down
            // with it (this is the "failed discovery degrades to the entry's
            // static models" rule; before the output cap, a windowed static
            // model never reached discovery at all, so the degradation was
            // only ever observable for window-less ones).
            if (auto resolved = ResolveDiscovered(*entry, ref.model, key)) {
                return resolved;
            }
            // Discovery could not fill the gaps (failed fetch, or the model is
            // a static twin its listing does not name): the metadata layer
            // gets its turn before the parsed self is handed back.
            if (entry->metadata) {
                EnrichFromMetadata(*entry, ref.model, key);
                if (const auto enriched = runtime_.overlay_.find(key);
                    enriched != runtime_.overlay_.end()) {
                    return enriched->second;
                }
            }
            return it->second;
        }
        // Metadata enrichment (docs/EXTENSIBILITY.md §2.13): a non-discover
        // entry's static model missing its window/cap consults the metadata
        // registry -- the auto default, declined per entry with
        // "metadata": false. Same overlay-copy rule as above.
        if (const ProviderEntry* entry = svc_.FindProviderLocked(ref.provider);
            entry != nullptr && entry->metadata) {
            EnrichFromMetadata(*entry, ref.model, key);
            if (const auto enriched = runtime_.overlay_.find(key); enriched != runtime_.overlay_.end()) {
                return enriched->second;
            }
        }
        return it->second;
    }

    // A static miss under a discover-entry may still be a model the backend
    // serves. Static entries elsewhere miss as today -- null, caller reports.
    if (const ProviderEntry* entry = svc_.FindProviderLocked(ref.provider);
        entry != nullptr && entry->discover) {
        return ResolveDiscovered(*entry, ref.model, key);
    }
    return nullptr;
}

std::shared_ptr<const ResolvedModel> ModelCatalogService::Resolution::ResolveDiscovered(
    const ProviderEntry& entry, const std::string& model_id, const std::string& ref_key) {
    // The fetch runs holding ONLY this provider's gate (see AcquireGate): a slow
    // catalog must not stall other config reads -- session creation, listings --
    // which take runtime_.mutex_ alone.
    const std::unique_lock<std::mutex> gate = AcquireGate(entry.name);

    // Double-check under the gate: another resolver may have fetched while we
    // waited, and its publish may already serve this model.
    if (const auto it = runtime_.overlay_.find(ref_key); it != runtime_.overlay_.end()) return it->second;

    std::shared_ptr<const CatalogRuntime::ProviderDiscovery> record =
        runtime_.discovery_record(entry.name);
    // A failed attempt is retried once its backoff elapsed: the negative cache
    // protects against a fetch per resolve, not against a backend that came up
    // after the first look.
    const bool failed_and_due =
        record && record->attempted && !record->ok &&
        std::chrono::steady_clock::now() - record->attempted_at >=
            runtime_.discovery_retry_backoff_;
    if (!record || !record->attempted || record->generation != runtime_.generation_ ||
        failed_and_due) {
        // The hook does network I/O -- no config lock held across it.
        const DiscoveryOutcome outcome = [&] {
            const Unlocked released(lock_);
            return svc_.FetchOutcome(entry);
        }();
        // A refresh may have published while we fetched; prefer its result.
        if (const auto it = runtime_.overlay_.find(ref_key); it != runtime_.overlay_.end()) return it->second;
        record = svc_.PublishLocked(entry, outcome);
    }

    if (!record->ok) return nullptr;

    if (const auto static_it = catalog_.models_.find(ref_key); static_it != catalog_.models_.end()) {
        // A STATIC model under a discover-entry: its own inheritance stands,
        // and the discovered window / output cap fill only the gaps that made
        // Resolve consult discovery at all -- each field independently, since
        // a model with a window but no cap reaches here too.
        const auto discovered = record->models.find(model_id);
        if (discovered != record->models.end()) {
            return runtime_.overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(
                                                        EnrichFromDiscovery(*static_it->second,
                                                                            discovered->second)))
                .first->second;
        }
        return static_it->second;  // nothing to enrich
    }
    if (const auto it = runtime_.overlay_.find(ref_key); it != runtime_.overlay_.end()) return it->second;
    return nullptr;  // discovery succeeded; the backend does not serve this id
}

std::shared_ptr<const ResolvedModel> ModelCatalogService::Resolution::ResolveVariant(
    const ProviderEntry& entry, const std::string& base_id, const std::string& variant_name,
    const std::string& ref_key) {
    // Served-from-overlay first: the sibling is built at most once per ref per
    // process, exactly like every other overlay value (RefreshDiscovery evicts
    // wholesale, so a rebuild after a refresh picks up a changed recipe).
    if (const auto it = runtime_.overlay_.find(ref_key); it != runtime_.overlay_.end()) {
        return it->second;
    }

    const std::string base_key = entry.name + "/" + base_id;

    // The variant must be DECLARED for this base. Static bases declared theirs
    // at parse time -- and their siblings were materialized into the catalog
    // then, so a static variant simply IS a catalog entry; check that first and
    // skip the rebuild entirely.
    if (const auto static_it = catalog_.models_.find(ref_key); static_it != catalog_.models_.end()) {
        return static_it->second;
    }

    // Everything else resolves through the base. Resolve the base WITHOUT the
    // suffix: a static base resolves as parsed (plus the discovery
    // enrichment, EnrichFromDiscovery), a discovered base runs the
    // lazy fetch (ResolveDiscovered unlocks around the hook, per its contract),
    // which is what makes `...@high` on a discover-entry fetch on first use.
    std::shared_ptr<const ResolvedModel> base;
    if (const auto base_it = catalog_.models_.find(base_key); base_it != catalog_.models_.end()) {
        // Enrich the static base from its discovery twin -- the sibling must
        // carry the same window/cap a session on the base would get, or a
        // /think switch silently drops compaction.
        if (entry.discover) {
            if (const auto record = runtime_.discovery_record(entry.name);
                record && record->ok) {
                const auto discovered = record->models.find(base_id);
                if (discovered != record->models.end()) {
                    base = std::make_shared<const ResolvedModel>(
                        EnrichFromDiscovery(*base_it->second, discovered->second));
                }
            }
        }
        if (!base) base = base_it->second;
    } else if (entry.discover) {
        base = ResolveDiscovered(entry, base_id, base_key);
    }
    if (base == nullptr) return nullptr;  // unknown base; caller reports the miss

    // The declared variant for this name. A static base: its recipe lived in
    // providers.json -- but parse-time synthesis put every declared static
    // sibling in the catalog (handled above), so reaching here with a static
    // base means the variant was NOT declared by config. It may still have an
    // AUTODETECTED ladder from metadata enrichment: consult the recorded
    // recipe (EnrichFromMetadata fills it on the base's first resolve; the
    // consult in the static branch below covers a `...@high` ref arriving
    // before any base resolve). A discovered base: the entry's recipe
    // (defaulting to the model's autodetected ladder), with what the
    // catalog's metadata advertised -- PublishLocked already recorded that
    // intersection in the discovery cache's variant list when it materialized
    // the base, so this is a lookup, never a re-derivation.
    std::optional<ModelVariant> declared;
    if (!entry.discover) {
        // A static twin's explicit `variants: []` opt-out (see VariantsOf's
        // identical check) applies here too: reaching this line already means
        // no static sibling matched `variant_name`, so an opted-out base must
        // answer "undeclared" rather than consulting the metadata ladder.
        if (base->variants_declared) return nullptr;
        // A `...@high` ref can arrive before the base itself was ever
        // resolved -- pull the metadata recipe in (no-op when it already
        // exists; the same per-provider-gate consult EnrichFromMetadata owns).
        if (entry.metadata) EnrichFromMetadata(entry, base_id, base_key);
        // The base must be the ENRICHED copy (window/cap/parameters), not the
        // parsed node the overlay also holds -- the sibling carries what a
        // session on the base would get, exactly as ResolveDiscovered's static
        // branch does for the discovery twin.
        if (const auto enriched_it = runtime_.overlay_.find(base_key);
            enriched_it != runtime_.overlay_.end()) {
            base = enriched_it->second;
        }
        const auto metadata_it = runtime_.metadata_variants_.find(base_key);
        if (metadata_it == runtime_.metadata_variants_.end()) return nullptr;
        const auto name_it = std::find_if(metadata_it->second.begin(),
                                          metadata_it->second.end(),
                                          [&](const ModelVariant& v) {
                                              return v.name == variant_name;
                                          });
        if (name_it == metadata_it->second.end()) return nullptr;
        return runtime_
            .overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(MakeVariantResolved(
                                           *base, *name_it, runtime_.variant_params_hook_)))
            .first->second;
    }
    // A static twin's explicit `variants: []` opt-out (see VariantsOf's
    // identical check) applies here too: reaching this line already means no
    // static sibling matched `variant_name`, so an opted-out base must answer
    // "undeclared" rather than falling through to the autodetected ladder.
    if (base->variants_declared) return nullptr;
    if (const auto record = runtime_.discovery_record(entry.name); record && record->ok) {
        const auto variants_it = record->variants_by_model.find(base_id);
        if (variants_it != record->variants_by_model.end()) {
            const auto name_it = std::find_if(variants_it->second.begin(),
                                              variants_it->second.end(),
                                              [&](const ModelVariant& v) {
                                                  return v.name == variant_name;
                                              });
            if (name_it != variants_it->second.end()) declared = *name_it;
        }
    }
    if (!declared) return nullptr;  // undeclared (or the model advertises no efforts)

    return runtime_
        .overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(MakeVariantResolved(
                                       *base, *declared, runtime_.variant_params_hook_)))
        .first->second;
}

void ModelCatalogService::Resolution::EnrichFromMetadata(const ProviderEntry& entry,
                                                         const std::string& model_id,
                                                         const std::string& ref_key) {
    // The config lock is held on entry: the hook snapshot and the
    // already-enriched checks below read mutex-guarded state directly.
    MetadataHook hook = runtime_.metadata_hook_;
    // Already enriched (or already answered "no answer"): the overlay or
    // tombstone IS the result; a second consult would repeat the Lookup on
    // every resolve of the same model.
    if (runtime_.overlay_.count(ref_key) != 0 || runtime_.metadata_tombstones_.count(ref_key) != 0) {
        return;
    }
    if (!hook) return;

    // The hook may do I/O (the modelsdev plugin reads its cache; a refresh
    // may hit the network). Hold ONLY this provider's gate across it -- the
    // same discipline ResolveDiscovered uses, for the same reason.
    const std::unique_lock<std::mutex> gate = AcquireGate(entry.name);

    // Double-check UNDER the gate (ResolveDiscovered does the same): resolvers
    // that all passed the unlocked check above queue on the gate, and without
    // this each of them re-consulted the catalog after the first one had
    // already answered -- N concurrent resolves of one model meant N Lookups.
    if (runtime_.overlay_.count(ref_key) != 0 || runtime_.metadata_tombstones_.count(ref_key) != 0) {
        return;
    }

    llm::MetadataQuery query;
    query.model_id = model_id;
    query.base_url = entry.connection.base_url;
    query.backend = entry.backend;
    query.provider_name = entry.name;
    query.metadata_provider = entry.metadata_provider;
    std::optional<llm::DiscoveredModel> answer;
    {
        const Unlocked released(lock_);  // the hook may do I/O
        try {
            answer = hook(query, entry.metadata_match);
        } catch (...) {
            // The hook's own contract is to degrade, not throw; this is
            // defense-in-depth, and it degrades the same way a nullopt would.
            answer = std::nullopt;
        }
    }
    // A refresh may have published while we consulted; prefer its result.
    if (const auto it = runtime_.overlay_.find(ref_key); it != runtime_.overlay_.end()) return;
    // The model was parsed away? It cannot be -- catalog_ is parse-time only
    // and ProvidersConfig has no removal path. The double-check under the gate
    // still re-verifies the tombstone in case a concurrent resolver enriched
    // terminal, so re-consulting never happens either way.
    if (runtime_.metadata_tombstones_.count(ref_key) != 0) return;

    if (!answer.has_value()) {
        runtime_.metadata_tombstones_.emplace(ref_key, true);
        return;
    }

    const auto static_it = catalog_.models_.find(ref_key);
    if (static_it == catalog_.models_.end()) {
        // Unreachable by construction (Lookup is keyed by an existing id) --
        // recorded as a miss rather than trusted.
        runtime_.metadata_tombstones_.emplace(ref_key, true);
        return;
    }
    // Gap-fill only: user-declared fields always win (EnrichFromDiscovery's
    // rule), and the overlay copy never mutates the parsed node.
    ResolvedModel enriched = EnrichFromDiscovery(*static_it->second, *answer);
    // The autodetected ladder from the metadata answer, for models that
    // declare no variants of their own -- the same recipe PublishLocked
    // computes for discovered models. Unknown effort names warn-and-skip so
    // an autodetected selector never resolves to a request identical to the
    // base model with no trace why.
    std::vector<ModelVariant> ladder;
    if (!enriched.variants_declared) {
        for (const auto& effort : llm::AdvertisedEfforts(answer->metadata)) {
            if (!IsKnownEffortName(effort)) {
                common::DefaultLogger().warn(
                    "providers.json: model '{}' advertises an unknown effort '{}' via metadata "
                    "enrichment; skipping that variant (known: high, low, max, medium, minimal, "
                    "none, xhigh)",
                    model_id, effort);
                continue;
            }
            ladder.push_back(ModelVariant{effort, {}, std::nullopt, std::nullopt});
        }
    }
    runtime_.metadata_variants_.emplace(ref_key, std::move(ladder));
    runtime_.overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(std::move(enriched)));
}

DiscoveryOutcome ModelCatalogService::FetchOutcome(const ProviderEntry& entry) const {
    DiscoveryOutcome outcome;
    // Snapshot the hook under runtime_.mutex_ (lock order gate -> mutex_, per
    // CatalogRuntime) rather than reading it bare: SetDiscoveryHook
    // writes it under that mutex, and a bare read of a std::function mid-assign
    // is UB, however unlikely the overlap. The copy is released before the
    // hook runs -- the network I/O must not hold any config lock.
    DiscoveryHook hook;
    MetadataHook metadata_hook;
    {
        std::lock_guard<std::mutex> lock(runtime_.mutex_);
        hook = runtime_.hook_;
        metadata_hook = runtime_.metadata_hook_;
    }
    if (!hook) {
        // Uninjected (a test or an unhosted config): report rather than treat
        // as success-with-zero-models, which would read as "backend serves
        // nothing".
        outcome.catalog_present = true;
        outcome.error = "no discovery hook set";
        return outcome;
    }
    outcome = hook(entry.backend, entry.connection);
    // Metadata defaults (docs/EXTENSIBILITY.md §2.13): the discovery listing
    // wins on ids and on anything IT reports, and the metadata layer fills
    // only what it leaves unset -- a listed model with no context window (the
    // vLLM gap this seam exists for) gets models.dev's window without a line
    // of config. Consulted HERE, inside the same per-provider gate the fetch
    // holds, so the fill is serialized with the fetch that feeds it and a
    // refreshed listing re-fills from the current metadata answer. A model
    // that declares its own `variants` keeps them: the ladder below is
    // skipped for any model whose metadata already advertises efforts, and
    // PublishLocked's declared-recipe precedence is untouched.
    if (outcome.ok && entry.metadata && metadata_hook && !outcome.models.empty()) {
        for (auto& model : outcome.models) {
            if (model.context_window_tokens.has_value() && model.max_tokens.has_value()) continue;
            llm::MetadataQuery query;
            query.model_id = model.id;
            query.base_url = entry.connection.base_url;
            query.backend = entry.backend;
            query.provider_name = entry.name;
            query.metadata_provider = entry.metadata_provider;
            try {
                if (auto fill = metadata_hook(query, entry.metadata_match);
                    fill.has_value()) {
                    if (!model.context_window_tokens.has_value() &&
                        fill->context_window_tokens.has_value()) {
                        model.context_window_tokens = fill->context_window_tokens;
                    }
                    if (!model.max_tokens.has_value() && fill->max_tokens.has_value()) {
                        model.max_tokens = fill->max_tokens;
                    }
                    // No ladder of its own from the listing: the metadata
                    // answer's `reasoning_options` IS the advertised effort
                    // set -- merged into the metadata the variant recipe
                    // reads below, never overriding what the listing said.
                    if (model.metadata.is_object() && !model.metadata.contains("reasoning") &&
                        fill->metadata.is_object() && fill->metadata.contains("reasoning_options") &&
                        !model.metadata.contains("reasoning_options")) {
                        model.metadata["reasoning_options"] = fill->metadata["reasoning_options"];
                    }
                }
            } catch (...) {
                // Enrichment must never fail a listing; the unfilled model
                // degrades exactly as if no metadata catalog existed.
            }
        }
    }
    return outcome;
}

std::shared_ptr<const CatalogRuntime::ProviderDiscovery> ModelCatalogService::PublishLocked(
    const ProviderEntry& entry, const DiscoveryOutcome& outcome) const {
    // Built privately and published last, as an immutable record: readers that
    // copied the previous record's pointer keep a consistent view, and a
    // failure partway never leaves a half-filled record visible.
    auto built = std::make_shared<CatalogRuntime::ProviderDiscovery>();
    CatalogRuntime::ProviderDiscovery& cache = *built;
    const auto publish = [&]() -> std::shared_ptr<const CatalogRuntime::ProviderDiscovery> {
        std::shared_ptr<const CatalogRuntime::ProviderDiscovery> record = std::move(built);
        runtime_.discovery_[entry.name] = record;
        return record;
    };
    cache.attempted = true;
    cache.attempted_at = std::chrono::steady_clock::now();
    cache.generation = runtime_.generation_;
    if (!outcome.catalog_present) {
        cache.ok = false;
        cache.error = "no model catalog registered for backend '" + entry.backend + "'";
        return publish();
    }
    if (!outcome.ok) {
        cache.ok = false;
        cache.error = outcome.error.empty() ? "catalog request failed" : outcome.error;
        return publish();
    }
    cache.ok = true;
    cache.error.clear();
    for (auto& model : outcome.models) {
        if (model.id.empty()) continue;  // unaddressable; not a config the caller wrote
        cache.models.insert_or_assign(model.id, std::move(model));
    }
    // The variant recipe per discovered model, computed ONCE here: the entry's
    // declared recipe (list of effort names or named bodies), or -- when the
    // entry declares none -- the model's own advertised ladder, verbatim. This
    // is the autodetection: glm-5.3-flash's catalog says supported_efforts
    // ["max","high","low"], so openrouter/z-ai/glm-5.3-flash gains @max, @high
    // and @low without a line of config. A model advertising no ladder (a bare
    // reasoning toggle, or nothing reasoning-shaped) gets none.
    // List-form entries carry no parameters of their own: MakeVariantResolved
    // derives them at sibling-build time, where the model's resolved
    // max_tokens is available for the anthropic budget clamp. Map-form
    // entries were validated at parse and pass through verbatim.
    for (const auto& [id, discovered] : cache.models) {
        std::vector<ModelVariant> declared = entry.variants;
        if (declared.empty()) {
            const auto efforts = llm::AdvertisedEfforts(discovered.metadata);
            if (efforts.empty()) continue;  // no ladder advertised: no variants
            // The model's OWN ladder, in the catalog's order -- the rungs the
            // backend says exist, autodetected. AdvertisedEfforts returns
            // non-empty only with at least one name, so the loop always fills.
            // Validated against the known vocabulary, unlike the config-file
            // path (ParseVariants) which already rejects unknown names at
            // parse time: an autodetected name outside it would otherwise
            // resolve as an addressable variant whose parameters are never
            // derived (MakeVariantResolved's IsKnownEffortName guard skips
            // it), sending a request byte-for-byte identical to the base
            // model with no warning that the selector did nothing.
            for (const auto& effort : efforts) {
                if (!IsKnownEffortName(effort)) {
                    common::DefaultLogger().warn(
                        "providers.json: model '{}' advertises an unknown effort '{}' via "
                        "discovery; skipping that variant (known: high, low, max, medium, "
                        "minimal, none, xhigh)",
                        id, effort);
                    continue;
                }
                declared.push_back(ModelVariant{effort, {}, std::nullopt, std::nullopt});
            }
        }
        // List-form entries carry no parameters of their own: MakeVariantResolved
        // derives them at sibling-build time, where the model's resolved
        // max_tokens is available for the anthropic budget clamp. Map-form
        // entries were validated at parse and pass through untouched.
        cache.variants_by_model.emplace(id, std::move(declared));
    }
    // Materialize every discovered model now: this is what puts them in
    // ModelRefs() and the /model picker's ListModels, and it means a later
    // Resolve() of any of
    // them is a lookup, never a rebuild. Values are written here -- once,
    // under the mutex, before any snapshot escapes -- and never mutated after.
    for (const auto& [id, discovered] : cache.models) {
        const ModelRef ref{entry.name, id};
        const std::string key = ref.str();
        if (catalog_.models_.count(key) || runtime_.overlay_.count(key)) continue;
        runtime_.overlay_.emplace(
            key, std::make_shared<const ResolvedModel>(BuildDiscoveredModel(entry, discovered)));
        // Variant siblings materialize lazily (ResolveVariant serves them from
        // overlay_ on first resolve) -- NOT here: a discover-entry lists
        // hundreds of models, and pre-building three siblings each would trade
        // the lazy-fetch design for an eager one. The ref-keyed lookup in
        // ResolveVariant makes the cost pay-per-use.
    }
    return publish();
}

std::vector<std::string> ModelCatalogService::ModelRefs() const {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    // Static ∪ cached-discovered ∪ cached-discovered variant siblings. All
    // three sources are individually sorted, but a concatenation interleaves
    // wrongly ("local/zdisc" sorts before "mixed/static1" yet lives in the
    // other map) -- so merge, skipping refs a static entry already provides
    // (an enriched static model and a parsed static variant sibling both live
    // in the catalog), and sort once at the end. A cold cache contributes
    // nothing: this is a listing, never a discovery trigger.
    std::vector<std::string> out;
    out.reserve(catalog_.models_.size() + runtime_.overlay_.size());
    for (const auto& [key, unused] : catalog_.models_) out.push_back(key);
    for (const auto& [key, unused] : runtime_.overlay_) {
        if (!catalog_.models_.count(key)) out.push_back(key);
    }
    // The variant siblings each materialized discovery-cache model carries.
    // VariantsOfLocked applies the same precedence the resolver does: a
    // static base's own declared siblings are catalog entries already (the
    // dedup below drops the overlap), and an explicit `variants: []` opt-out
    // suppresses the autodetected ladder. The discovered-model guard skips
    // enriched static twins (their siblings come from the catalog) and a
    // variant sibling already materialized into the overlay re-derives its
    // base, which keeps a later /think switch from changing the listing.
    for (const auto& [key, unused] : runtime_.overlay_) {
        (void)unused;
        const auto slash = key.find('/');
        if (slash == std::string::npos) continue;
        const std::string provider = key.substr(0, slash);
        const std::string model_id = SplitVariantSuffix(key.substr(slash + 1)).first;
        const auto record = runtime_.discovery_record(provider);
        if (!record || !record->ok) continue;
        if (!record->models.count(model_id)) continue;  // an enriched static twin
        for (const auto& name : VariantsOfLocked(provider, model_id)) {
            out.push_back(provider + "/" + model_id + "@" + name);
        }
    }

    // The picker filters (opencode compat): whitelist keeps only the listed
    // model ids for that provider, then blacklist removes the listed ones.
    // Resolution is untouched -- this is a listing concern, not an access rule.
    // Providers_ is keyed by name and stable after parse, so the lookups are
    // safe without a second lock.
    bool filtered = false;
    for (const auto& [name, entry] : catalog_.providers_) {
        if (!entry.whitelist.empty() || !entry.blacklist.empty()) filtered = true;
    }
    if (filtered) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const std::string& key) {
                                     const auto slash = key.find('/');
                                     if (slash == std::string::npos) return false;
                                     const auto entry = catalog_.providers_.find(key.substr(0, slash));
                                     if (entry == catalog_.providers_.end()) return false;
                                     // A variant ref filters by its BASE id: hiding
                                     // a model must hide its variants too, and a
                                     // whitelist of base ids must keep them.
                                     const std::string model_id =
                                         SplitVariantSuffix(key.substr(slash + 1)).first;
                                     if (!entry->second.whitelist.empty() &&
                                         std::find(entry->second.whitelist.begin(),
                                                   entry->second.whitelist.end(),
                                                   model_id) == entry->second.whitelist.end()) {
                                         return true;
                                     }
                                     return std::find(entry->second.blacklist.begin(),
                                                      entry->second.blacklist.end(),
                                                      model_id) != entry->second.blacklist.end();
                                 }),
                    out.end());
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

DiscoveryStatus ModelCatalogService::discovery_status(const std::string& provider_name) const {
    std::lock_guard<std::mutex> lock(runtime_.mutex_);
    const auto record = runtime_.discovery_record(provider_name);
    if (!record) return {};
    return DiscoveryStatus{record->attempted, record->ok, record->error};
}

std::vector<DiscoveryRefreshOutcome> ModelCatalogService::RefreshDiscovery() const {
    std::vector<DiscoveryRefreshOutcome> outcomes;
    for (const auto& [name, entry] : catalog_.providers_) {
        if (!entry.discover) continue;
        // Fetch holding only this provider's gate: refreshing a hung catalog
        // costs this provider its 10 s, never the process its config reads,
        // and distinct providers refresh in parallel. gate_for looks the gate
        // up under runtime_.mutex_ and releases it before the gate is taken
        // (lock order gate -> mutex_): a bare gates_ lookup here raced a
        // concurrent resolver inserting another provider's gate.
        std::lock_guard<std::mutex> gate_lock(runtime_.gate_for(name).mtx);
        const DiscoveryOutcome outcome = FetchOutcome(entry);

        std::lock_guard<std::mutex> lock(runtime_.mutex_);
        // A FAILED refresh over a previously good fetch keeps the good data:
        // the backend being down for this Ctrl+R must not empty the picker or
        // make models that resolved a minute ago "unknown". The failure is
        // reported in the outcome (and logged), not stored over the cache.
        if (!outcome.ok) {
            const auto previous = runtime_.discovery_record(name);
            if (previous && previous->ok) {
                const std::string error =
                    outcome.error.empty() ? "catalog request failed" : outcome.error;
                common::DefaultLogger().warn(
                    "providers.json: discovery refresh for '{}' failed ({}); keeping the "
                    "previous {} models",
                    name, error, previous->models.size());
                outcomes.push_back(
                    DiscoveryRefreshOutcome{name, false, error, previous->models.size()});
                continue;
            }
        }
        // Evict and republish atomically, under one lock acquisition: the
        // listing switches to the fresh data the moment the lock is released,
        // and readers holding snapshots of the old overlays keep those objects
        // alive -- replacing an entry never pulls it out from under a holder.
        for (auto it = runtime_.overlay_.begin(); it != runtime_.overlay_.end();) {
            if (it->second->ref.provider == name) {
                it = runtime_.overlay_.erase(it);
            } else {
                ++it;
            }
        }
        const auto record = PublishLocked(entry, outcome);
        outcomes.push_back(
            DiscoveryRefreshOutcome{name, record->ok, record->error, record->models.size()});
    }
    return outcomes;
}

}  // namespace minimal::config
