#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::config {

struct ProviderEntry {
    std::string name;      // matched against a preset's `provider` field
    std::string provider;  // kind string for ProviderFactoryRegistry -- not validated here
    std::string base_url;
    std::string api_key;  // resolved once at load from api_key or api_key_env
    std::string model_name;
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<std::int64_t> context_window_tokens;
    // Typed rather than left to `parameters`, because the serializer treats
    // max_tokens as a structural field and erases whatever `parameters` put
    // there -- so this was the only route to an output cap, and it did not
    // exist. Unset means "send no cap and let the backend decide", which is the
    // right default for a local llama.cpp and wrong for a metered API.
    std::optional<int> max_tokens;
};

struct ProvidersConfig {
    std::vector<ProviderEntry> entries;

    const ProviderEntry* Find(const std::string& name) const;
};

// Pure: throws std::runtime_error on a missing required field (name, provider,
// base_url, model_name), a duplicate `name` across entries, both api_key and
// api_key_env set, or api_key_env naming an unset environment variable. Neither
// api_key nor api_key_env set is NOT an error -- some local servers need no key.
ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or isn't
// valid JSON, or per ParseProvidersConfig above.
ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path);

}  // namespace minimal::config
