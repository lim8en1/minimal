#pragma once

// Constructed provider instances, SHARED across sessions (ARCHITECTURE.md, llm
// section, "Two registries, deliberately distinct"). ProviderFactoryRegistry is
// kind-string -> factory; this is the instance cache on top of it.
//
// WHY SHARING IS SAFE: the provider thread-safety contract requires
// implementations to hold no mutable state across calls -- one CURL handle per
// call, per-request reassembly state local to the call. Instances are therefore
// freely shareable, and constructing one per session is redundant work.
//
// WHY THE KEY IS THE ENTRY NAME: providers.json entries are immutable at
// runtime (config is read once at startup; there is no hot-reload), so the
// entry name uniquely determines the connection. The API key is deliberately
// NOT part of the key -- hashing a secret into a map key invites leaking it
// through logs and dumps. The generation counter exists for the day config
// reload does; until then it is always zero and the field is documentation
// rather than mechanism.
//
// The cache holds instances for the process lifetime. Providers are tiny
// (config plus nothing else), the plugin handles they live behind are never
// unloaded anyway, and the alternative -- refcounted eviction -- is machinery
// for a resource that does not strain.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"

namespace minimal::agent {

class ProviderInstanceCache {
public:
    explicit ProviderInstanceCache(const llm::ProviderFactoryRegistry& factories,
                                   common::Logger& logger)
        : factories_(factories), logger_(logger) {}

    // Shared instance for one resolved providers.json entry. Returns nullptr
    // when the kind is unregistered or a factory fails to construct -- the
    // caller reports it with the entry's name attached, which is the useful
    // diagnostic, so this stays a value rather than an exception.
    //
    // Thread-safe: called from SessionRegistry::CreateResolved for every new
    // session, on that session's creating thread -- several workers can be
    // spawning subagents concurrently.
    std::shared_ptr<llm::LlmProvider> GetOrCreate(std::string_view provider_entry_name,
                                                  std::string_view kind,
                                                  const llm::ProviderConfig& config);

    // Bumped when a future config reload invalidates every cached instance at
    // once. Always zero today; see the class comment.
    std::uint64_t generation() const { return generation_; }

private:
    const llm::ProviderFactoryRegistry& factories_;
    common::Logger& logger_;
    std::mutex mu_;  // GetOrCreate mutates from multiple session-creating threads
    // map, not unordered_map: provider entries are few, and a stable iteration
    // order keeps any future diagnostic honest.
    std::map<std::pair<std::uint64_t, std::string>, std::shared_ptr<llm::LlmProvider>>
        instances_;
    std::uint64_t generation_ = 0;
};

}  // namespace minimal::agent
