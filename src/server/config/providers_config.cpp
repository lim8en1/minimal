#include "minimal/config/providers_config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace minimal::config {

namespace {

std::string RequireString(const nlohmann::json& entry, const std::string& key, std::size_t index) {
    if (!entry.contains(key) || !entry.at(key).is_string() || entry.at(key).get<std::string>().empty()) {
        throw std::runtime_error("providers.json: entry #" + std::to_string(index) +
                                  " is missing required field '" + key + "'");
    }
    return entry.at(key).get<std::string>();
}

ProviderEntry ParseEntry(const nlohmann::json& entry, std::size_t index) {
    ProviderEntry result;
    result.name = RequireString(entry, "name", index);
    result.provider = RequireString(entry, "provider", index);
    result.base_url = RequireString(entry, "base_url", index);
    result.model_name = RequireString(entry, "model_name", index);

    const bool has_api_key = entry.contains("api_key") && !entry.at("api_key").get<std::string>().empty();
    const bool has_api_key_env = entry.contains("api_key_env") && !entry.at("api_key_env").get<std::string>().empty();
    if (has_api_key && has_api_key_env) {
        throw std::runtime_error("providers.json: entry '" + result.name + "' sets both api_key and api_key_env");
    }
    if (has_api_key) {
        result.api_key = entry.at("api_key").get<std::string>();
    } else if (has_api_key_env) {
        const std::string env_name = entry.at("api_key_env").get<std::string>();
        const char* value = std::getenv(env_name.c_str());
        if (!value || !*value) {
            throw std::runtime_error("providers.json: entry '" + result.name + "' names api_key_env '" + env_name +
                                      "' which is unset");
        }
        result.api_key = value;
    }

    if (entry.contains("parameters")) result.parameters = entry.at("parameters");
    if (entry.contains("max_tokens")) {
        result.max_tokens = entry.at("max_tokens").get<int>();
    }
    if (entry.contains("context_window_tokens")) {
        result.context_window_tokens = entry.at("context_window_tokens").get<std::int64_t>();
    }
    return result;
}

}  // namespace

const ProviderEntry* ProvidersConfig::Find(const std::string& name) const {
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc) {
    if (!doc.is_array()) throw std::runtime_error("providers.json: expected a top-level array");

    ProvidersConfig config;
    std::unordered_set<std::string> seen_names;
    for (std::size_t i = 0; i < doc.size(); ++i) {
        ProviderEntry entry;
        try {
            entry = ParseEntry(doc.at(i), i);
        } catch (const nlohmann::json::exception& error) {
            throw std::runtime_error("providers.json: entry #" + std::to_string(i) + ": " + error.what());
        }
        if (!seen_names.insert(entry.name).second) {
            throw std::runtime_error("providers.json: duplicate provider name '" + entry.name + "'");
        }
        config.entries.push_back(std::move(entry));
    }
    return config;
}

ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("providers.json not found: " + path.string());
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("providers.json: invalid JSON: " + std::string(error.what()));
    }
    return ParseProvidersConfig(doc);
}

}  // namespace minimal::config
