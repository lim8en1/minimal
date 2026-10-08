#include "minimal/agent/provider_resolver.hpp"

#include <algorithm>

#include <stdexcept>
#include <utility>

namespace minimal::agent {

ProviderResolver::ProviderResolver(const config::ProvidersConfig& catalog,
                                   const llm::ProviderFactoryRegistry& factories,
                                   common::Logger& logger)
    : catalog_(catalog), factories_(factories), cache_(factories, logger) {}

AgentPreset ProviderResolver::ResolveModel(AgentPreset preset,
                                           const std::optional<common::ModelRef>& model) const {
    if (!model) return preset;
    const auto resolved = catalog_.Resolve(*model);
    if (resolved == nullptr) {
        throw std::runtime_error("unknown model '" + model->str() + "'; providers.json defines " +
                                 config::DescribeModels(catalog_) +
                                 config::DescribeDiscovery(catalog_, model->provider));
    }
    ApplyModel(preset, *resolved);
    return preset;
}

std::shared_ptr<llm::LlmProvider> ProviderResolver::Acquire(const AgentPreset& preset) {
    // A preset only reaches here with an entry that parsed, so an unregistered
    // kind means a plugin is missing or disabled -- named, because the symptom
    // otherwise is a turn that fails for no stated reason.
    if (!factories_.Has(preset.provider_kind)) {
        throw std::runtime_error("agent preset '" + preset.name + "' needs provider kind '" +
                                 preset.provider_kind + "', which no loaded plugin registered");
    }
    auto provider =
        cache_.GetOrCreate(preset.model_ref.provider, preset.provider_kind, preset.provider);
    if (provider == nullptr) {
        throw std::runtime_error("provider kind '" + preset.provider_kind +
                                 "' failed to construct");
    }
    return provider;
}

std::optional<ResolvedLlm> ProviderResolver::ResolveLlm(std::string_view provider_model,
                                                        std::string& error) {
    const auto ref = common::SplitModelRef(provider_model);
    if (!ref) {
        error = "'" + std::string(provider_model) + "' is not a <provider>/<model> reference";
        return std::nullopt;
    }
    auto model = catalog_.Resolve(*ref);
    if (model == nullptr) {
        error = "'" + std::string(provider_model) + "' resolves to nothing in providers.json";
        return std::nullopt;
    }
    auto provider = cache_.GetOrCreate(ref->provider, model->backend, model->provider);
    if (provider == nullptr) {
        error = "'" + std::string(provider_model) + "' failed to construct a provider";
        return std::nullopt;
    }
    return ResolvedLlm{std::move(provider), std::move(model)};
}

std::string ProviderResolver::ModelRefProblem(std::string_view provider_model) const {
    const auto ref = common::SplitModelRef(provider_model);
    if (!ref) return "'" + std::string(provider_model) + "' is not a <provider>/<model> reference";
    const auto model = catalog_.Resolve(*ref);
    if (model == nullptr) {
        return "'" + std::string(provider_model) + "' resolves to nothing in providers.json";
    }
    const auto kinds = factories_.kinds();
    if (std::find(kinds.begin(), kinds.end(), model->backend) == kinds.end()) {
        return "'" + std::string(provider_model) + "' needs backend '" + model->backend +
               "', which no loaded plugin registers";
    }
    return {};
}

bool ProviderResolver::SameConnection(const AgentPreset& a, const AgentPreset& b) {
    return a.model_ref.provider == b.model_ref.provider && a.provider_kind == b.provider_kind &&
           a.provider.base_url == b.provider.base_url && a.provider.api_key == b.provider.api_key;  // same reference, not same value
}

}  // namespace minimal::agent
