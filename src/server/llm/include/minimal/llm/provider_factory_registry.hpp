#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/llm/provider.hpp"
#include "minimal/plugins/attributed_registry.hpp"

namespace minimal::llm {

// Maps a providers.json `provider` string to a factory. This is why there is no
// ProviderKind enum: an enum could not name a kind supplied by a plugin core has
// never heard of, and providers.json already carries the value as a string.
//
// Distinct from agent::ProviderInstanceCache: this holds kind-string ->
// factory, that holds constructed instances shared across sessions.
class ProviderFactoryRegistry
    : public plugins::AttributedRegistry<std::function<std::unique_ptr<LlmProvider>(
          const ProviderConfig&)>> {
public:
    using Factory = std::function<std::unique_ptr<LlmProvider>(const ProviderConfig&)>;

    // False for an empty factory or a kind already taken. FIRST REGISTRATION
    // WINS, and the loser is recorded in collisions() naming both plugins --
    // which backend serves `openai` must never silently depend on load order.
    bool Register(std::string kind, Factory factory);

    // Returns nullptr for an unregistered kind. A providers.json entry naming a
    // kind no plugin supplied is a startup sanity-check failure, not a crash.
    std::unique_ptr<LlmProvider> Create(std::string_view kind, const ProviderConfig& config) const;
};

}  // namespace minimal::llm
