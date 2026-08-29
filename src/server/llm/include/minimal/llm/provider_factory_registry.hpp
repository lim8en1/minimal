#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/llm/provider.hpp"

namespace minimal::llm {

// Maps a providers.json `provider` string to a factory. This is why there is no
// ProviderKind enum: an enum could not name a kind supplied by a plugin core has
// never heard of, and providers.json already carries the value as a string.
//
// Distinct from the (future) ProviderInstanceCache, which the two were once
// conflated under one name: this holds kind-string -> factory, that holds
// constructed instances shared across concurrent sessions.
class ProviderFactoryRegistry {
public:
    using Factory = std::function<std::unique_ptr<LlmProvider>(const ProviderConfig&)>;

    // Returns false if `kind` was already registered -- the caller decides
    // whether that is fatal, since the loader wants to report which two plugins
    // collided rather than throw from inside a plugin's registration callback.
    bool Register(std::string kind, Factory factory);

    bool Has(std::string_view kind) const;

    // Returns nullptr for an unregistered kind. A providers.json entry naming a
    // kind no plugin supplied is a startup sanity-check failure, not a crash.
    std::unique_ptr<LlmProvider> Create(std::string_view kind, const ProviderConfig& config) const;

    // Sorted, for deterministic diagnostics and /providers output.
    std::vector<std::string> kinds() const;

private:
    std::unordered_map<std::string, Factory> factories_;
};

}  // namespace minimal::llm
