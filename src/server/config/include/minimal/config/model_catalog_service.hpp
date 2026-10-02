#pragma once

// The runtime half of the model catalog: lazy discovery fetches (single-flight
// per provider), enrichment from discovery and from the metadata catalog,
// variant materialisation, and the Resolve / ModelRefs / VariantsOf /
// discovery_status / RefreshDiscovery answers built on them.
//
// It reads the immutable parse result (ImmutableCatalog) it was bound to and
// owns all the mutable state (CatalogRuntime). ProvidersConfig owns one and
// delegates to it; the public ProvidersConfig API is unchanged.
//
// THREAD-SAFETY: called from RPC threads (ListModels), session workers, the
// command pool and the discovery warm-up thread at once. Everything mutable
// lives behind CatalogRuntime::mutex_ plus the per-provider fetch gates (lock
// order: gate -> mutex_); hooks run holding only a gate, never mutex_.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "minimal/config/catalog_runtime.hpp"
#include "minimal/config/providers_types.hpp"

namespace minimal::config {

class ModelCatalogService {
public:
    // Bound to `catalog` by reference: the catalog must outlive the service and
    // stay at one address. Neither copyable nor movable for that reason.
    explicit ModelCatalogService(const ImmutableCatalog& catalog) : catalog_(catalog) {}
    ModelCatalogService(const ModelCatalogService&) = delete;
    ModelCatalogService& operator=(const ModelCatalogService&) = delete;

    std::shared_ptr<const ResolvedModel> Resolve(const ModelRef& ref) const;
    std::vector<std::string> ModelRefs() const;
    std::vector<std::string> VariantsOf(const ModelRef& ref) const;
    DiscoveryStatus discovery_status(const std::string& provider_name) const;
    std::vector<DiscoveryRefreshOutcome> RefreshDiscovery() const;

    void SetDiscoveryHook(DiscoveryHook hook);
    void SetDiscoveryRetryBackoff(std::chrono::milliseconds backoff);
    void SetMetadataHook(MetadataHook hook);
    void SetVariantParamsHook(VariantParamsHook hook);

    // Takes over `other`'s runtime state (the catalog binding is untouched).
    // Config load only, before any thread can see either side.
    void MoveRuntimeFrom(ModelCatalogService&& other) {
        runtime_.MoveFrom(std::move(other.runtime_));
    }

private:
    const ImmutableCatalog& catalog_;
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
    // Builds the provider's new immutable discovery record from one outcome, swaps it in, and materializes every discovered
    // model into the overlay, so a later Resolve of any of them is a lookup,
    // never a rebuild. runtime_.mutex_ held.
    std::shared_ptr<const CatalogRuntime::ProviderDiscovery> PublishLocked(
        const ProviderEntry& entry, const DiscoveryOutcome& outcome) const;
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

}  // namespace minimal::config
