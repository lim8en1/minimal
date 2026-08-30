#include "minimal/config/providers_config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace minimal::config {
namespace {

std::string RequireString(const nlohmann::json& node, const std::string& key,
                          const std::string& where) {
    if (!node.contains(key) || !node.at(key).is_string() ||
        node.at(key).get<std::string>().empty()) {
        throw std::runtime_error("providers.json: " + where + " is missing required field '" + key +
                                 "'");
    }
    return node.at(key).get<std::string>();
}

// api_key or api_key_env, never both. Resolved once here so nothing downstream
// touches the environment -- and so an unset variable fails at startup rather
// than on the first request.
std::string ResolveApiKey(const nlohmann::json& provider, const std::string& where) {
    const bool has_key = provider.contains("api_key") && provider.at("api_key").is_string() &&
                         !provider.at("api_key").get<std::string>().empty();
    const bool has_env = provider.contains("api_key_env") &&
                         provider.at("api_key_env").is_string() &&
                         !provider.at("api_key_env").get<std::string>().empty();
    if (has_key && has_env) {
        throw std::runtime_error("providers.json: " + where + " sets both api_key and api_key_env");
    }
    if (has_key) return provider.at("api_key").get<std::string>();
    if (!has_env) return {};  // a local server needs none

    const std::string name = provider.at("api_key_env").get<std::string>();
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("providers.json: " + where + " names api_key_env '" + name +
                                 "' which is unset");
    }
    return value;
}

// The model's value if it has one, else the provider's. Shallow by design: a
// model overriding `parameters` replaces the object rather than merging keys into
// it, so what a model declares is exactly what it sends. Deep-merging would make
// a model's stated settings depend on a provider block elsewhere in the file.
nlohmann::json InheritParameters(const nlohmann::json& provider, const nlohmann::json& model) {
    if (model.contains("parameters") && model.at("parameters").is_object()) {
        return model.at("parameters");
    }
    if (provider.contains("parameters") && provider.at("parameters").is_object()) {
        return provider.at("parameters");
    }
    return nlohmann::json::object();
}

template <typename T>
std::optional<T> InheritNumber(const nlohmann::json& provider, const nlohmann::json& model,
                               const char* key) {
    if (model.contains(key) && model.at(key).is_number()) return model.at(key).get<T>();
    if (provider.contains(key) && provider.at(key).is_number()) return provider.at(key).get<T>();
    return std::nullopt;
}

}  // namespace

void ProvidersConfig::Add(const ModelRef& ref, ResolvedModel resolved) {
    models_.insert_or_assign(ref.str(), std::move(resolved));
}

const ResolvedModel* ProvidersConfig::Resolve(const ModelRef& ref) const {
    const auto it = models_.find(ref.str());
    return it == models_.end() ? nullptr : &it->second;
}

std::vector<std::string> ProvidersConfig::ModelRefs() const {
    std::vector<std::string> out;
    out.reserve(models_.size());
    for (const auto& [key, unused] : models_) out.push_back(key);
    return out;
}

ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc) {
    // The pre-`models` schema was a top-level array of flat provider+model
    // records. Named explicitly rather than allowed to fail as "expected an
    // object": someone hitting this has a working file from a previous build and
    // needs to know what changed, not that their JSON is the wrong type.
    if (doc.is_array()) {
        throw std::runtime_error(
            "providers.json: this is the old format (a top-level array of provider entries). "
            "Providers are now an object keyed by name, each with a nested \"models\" object, and "
            "models are addressed as <provider>/<model>. See providers.example.json beside this "
            "file.");
    }
    if (!doc.is_object()) {
        throw std::runtime_error("providers.json: expected a top-level object with \"providers\"");
    }

    ProvidersConfig config;
    if (!doc.contains("providers")) return config;  // {} is a valid empty config
    const auto& providers = doc.at("providers");
    if (!providers.is_object()) {
        throw std::runtime_error("providers.json: \"providers\" must be an object keyed by name");
    }

    for (const auto& [provider_name, provider] : providers.items()) {
        const std::string where = "provider '" + provider_name + "'";
        if (!provider.is_object()) {
            throw std::runtime_error("providers.json: " + where + " must be an object");
        }

        const std::string backend = RequireString(provider, "backend", where);
        const std::string url = RequireString(provider, "url", where);
        const std::string api_key = ResolveApiKey(provider, where);

        if (!provider.contains("models") || !provider.at("models").is_object() ||
            provider.at("models").empty()) {
            // A provider with no models is unaddressable -- nothing could name it
            // -- so it is a mistake rather than an empty-but-valid state.
            throw std::runtime_error("providers.json: " + where +
                                     " has no \"models\"; a provider serving no model cannot be "
                                     "addressed as <provider>/<model>");
        }

        for (const auto& [model_name, model] : provider.at("models").items()) {
            if (!model.is_object()) {
                throw std::runtime_error("providers.json: model '" + provider_name + "/" +
                                         model_name + "' must be an object");
            }
            if (model_name.find('/') != std::string::npos ||
                provider_name.find('/') != std::string::npos) {
                // Otherwise the reference cannot be parsed back apart.
                throw std::runtime_error("providers.json: '" + provider_name + "/" + model_name +
                                         "' contains a '/' in a name, which makes the reference "
                                         "ambiguous");
            }

            ResolvedModel resolved;
            resolved.backend = backend;
            resolved.provider.base_url = url;
            resolved.provider.api_key = api_key;
            resolved.provider.model_name = model_name;
            resolved.parameters = InheritParameters(provider, model);
            resolved.max_tokens = InheritNumber<int>(provider, model, "max_tokens");
            resolved.context_window_tokens =
                InheritNumber<std::int64_t>(provider, model, "context_window_tokens");
            config.Add(ModelRef{provider_name, model_name}, std::move(resolved));
        }
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
