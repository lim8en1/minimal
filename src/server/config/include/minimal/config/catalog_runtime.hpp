#pragma once

// The MUTABLE half of ProvidersConfig: everything the lazy discovery and
// enrichment machinery caches or injects at runtime, grouped so the split from
// the immutable parse result (ProvidersConfig::ImmutableCatalog) is structural,
// not a naming convention. Refresh replaces overlay entries wholesale -- old
// objects stay alive through any shared_ptr still holding them -- so a Resolve
// snapshot remains valid across refreshes.
//
// Lock order is always FetchGate::mtx -> mutex_, never the reverse.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "minimal/config/providers_types.hpp"

namespace minimal::config {

struct CatalogRuntime {
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

    struct ProviderDiscovery {
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
    // One IMMUTABLE record per provider, published by swapping the pointer: a
    // reader copies the shared_ptr under mutex_ and then reads it freely, and a
    // refresh builds a new record instead of mutating the one readers hold.
    // Absent until the provider's first attempt.
    mutable std::map<std::string, std::shared_ptr<const ProviderDiscovery>> discovery_;
    // mutex_ held.
    std::shared_ptr<const ProviderDiscovery> discovery_record(const std::string& provider) const {
        const auto it = discovery_.find(provider);
        return it == discovery_.end() ? nullptr : it->second;
    }
    // Discovered models, and overlay copies of static models enriched with
    // a discovered context window. Keyed "<provider>/<model>".
    mutable std::map<std::string, std::shared_ptr<const ResolvedModel>> overlay_;
    // The variant recipes metadata enrichment contributed, per
    // "<provider>/<model>" base key -- the static-side twin of
    // ProviderDiscovery::variants_by_model. Written by the lazy metadata
    // enrichment in Resolve/ResolveVariant, read by VariantsOfLocked and
    // ResolveVariant. Precedence makes this safe to fill eagerly or
    // lazily: a parse-time declared sibling lives in the catalog and wins
    // regardless of what lands here, and `variants_declared` bases never
    // consult it. An entry in metadata_tombstones_ has already consulted
    // the hook and gotten "no answer" -- recorded so a miss costs one
    // Lookup, not one per resolve.
    mutable std::map<std::string, std::vector<ModelVariant>> metadata_variants_;
    mutable std::map<std::string, bool> metadata_tombstones_;  // ref key -> miss recorded

    // Takes over `other`'s runtime state. The ONE place move semantics live:
    // ProvidersConfig's move-assign delegates here, so a member added above and
    // forgotten below is a single, local omission (it already happened once --
    // the hooks and tombstones were dropped) instead of one buried in a
    // hand-written assignment elsewhere. Moves happen only during config load,
    // before any thread can see either side; both mutexes are taken anyway.
    void MoveFrom(CatalogRuntime&& other) {
        std::lock_guard<std::mutex> a(mutex_);
        std::lock_guard<std::mutex> b(other.mutex_);
        gates_ = std::move(other.gates_);
        hook_ = std::move(other.hook_);
        metadata_hook_ = std::move(other.metadata_hook_);
        variant_params_hook_ = std::move(other.variant_params_hook_);
        generation_ = other.generation_;
        discovery_retry_backoff_ = other.discovery_retry_backoff_;
        discovery_ = std::move(other.discovery_);
        overlay_ = std::move(other.overlay_);
        metadata_variants_ = std::move(other.metadata_variants_);
        metadata_tombstones_ = std::move(other.metadata_tombstones_);
    }
};

}  // namespace minimal::config
