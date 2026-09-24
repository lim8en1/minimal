#pragma once

// The one path from a model reference to a live provider instance. Every
// caller -- session creation, /model and /agent, per-turn overrides,
// settings.small_model, the session-support service -- goes through here, so
// "which instance serves this preset" and "did the connection change" are
// decided in exactly one place.
//
// Instances come from the shared ProviderInstanceCache (keyed by providers.json
// entry name). Thread-safe: the catalog is read-only after load and the cache
// locks internally.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/provider_instance_cache.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"

namespace minimal::agent {

// A resolved model plus the shared instance that serves it.
struct ResolvedLlm {
    std::shared_ptr<llm::LlmProvider> provider;
    std::shared_ptr<const config::ResolvedModel> model;
};

class ProviderResolver {
public:
    ProviderResolver(const config::ProvidersConfig& catalog,
                     const llm::ProviderFactoryRegistry& factories, common::Logger& logger);

    // `preset` with `model` applied (ApplyModel), or unchanged when `model` is
    // unset. Throws std::runtime_error naming what providers.json offers when
    // the ref does not resolve.
    AgentPreset ResolveModel(AgentPreset preset, const std::optional<common::ModelRef>& model) const;

    // The shared instance for `preset`'s providers.json entry. Throws
    // std::runtime_error when no plugin registered the kind or the factory
    // failed to construct.
    std::shared_ptr<llm::LlmProvider> Acquire(const AgentPreset& preset);

    // "<provider>/<model>" (slash-bearing model ids allowed) -> model and
    // instance. nullopt with `error` set on a malformed ref, an unknown model,
    // or a construction failure; callers add their own context when logging.
    std::optional<ResolvedLlm> ResolveLlm(std::string_view provider_model, std::string& error);

    // Whether two presets are served by the same instance: same entry, kind,
    // endpoint and key. The entry name matters on its own -- each entry's
    // instance carries that entry's resolved config.
    static bool SameConnection(const AgentPreset& a, const AgentPreset& b);

    const config::ProvidersConfig& catalog() const { return catalog_; }
    ProviderInstanceCache& cache() { return cache_; }

private:
    const config::ProvidersConfig& catalog_;
    const llm::ProviderFactoryRegistry& factories_;
    ProviderInstanceCache cache_;
};

}  // namespace minimal::agent
