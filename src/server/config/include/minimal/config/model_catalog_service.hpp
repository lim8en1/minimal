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
    // One Resolve call's working state: owns the unique_lock on runtime_.mutex_
    // for the call and the only code that releases it (around the per-provider
    // gate and around hook I/O), so the lock never appears in a signature and a
    // release is a lexical scope that re-locks on every exit path. Defined in
    // the .cpp; holds the discover / variant / metadata-enrichment halves of a
    // resolve.
    class Resolution;
};

}  // namespace minimal::config
