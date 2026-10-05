#include "opencode_providers.hpp"

#include "minimal/common/logger.hpp"

namespace minimal::opencode_compat {

// opencode's provider block, mapped onto core's:
//   options.baseURL -> url        (only when url absent)
//   options.apiKey  -> api_key    (only when api_key is absent)
//   options.headers -> headers (core then normalizes either header shape)
//   models.<id>.limit.context  -> context_window_tokens
//   models.<id>.limit.output   -> max_tokens
//   models.<id>.options -> parameters  (opencode's AI SDK spreads a model's
//          `options` into the request body; `parameters` is minimal's verbatim
//          passthrough. Keys already present in a native `parameters` object
//          win, same precedence rule as the provider-level fields above.)
//   models.<id>.name -> dropped (the picker shows the ref id; no display name)
//   npm -> consumed: the AI-SDK package selector maps to a backend kind, and
//          the one package that names a wire shape (@ai-sdk/openai-compatible)
//          is exactly what backend "openai" already speaks.
nlohmann::json TranslateOpencodeProvider(const nlohmann::json& provider, const std::string& where) {
    if (!provider.is_object()) return provider;
    nlohmann::json out = provider;

    if (out.contains("npm") && out.at("npm").is_string()) {
        const std::string npm = out.at("npm").get<std::string>();
        if (!out.contains("backend")) {
            if (npm == "@ai-sdk/openai-compatible") {
                out["backend"] = "openai";
            } else {
                minimal::common::DefaultLogger().warn(
                    "providers.json: {} names npm '{}' with no backend; add \"backend\" "
                    "(the npm package does not select the wire shape here)",
                    where, npm);
            }
        }
        out.erase("npm");
    }

    if (out.contains("options") && out.at("options").is_object()) {
        const auto& options = out.at("options");
        if (options.contains("baseURL") && options.at("baseURL").is_string() &&
            !out.contains("url")) {
            out["url"] = options.at("baseURL");
        }
        if (options.contains("apiKey") && options.at("apiKey").is_string() &&
            !out.contains("api_key")) {
            out["api_key"] = options.at("apiKey");
        }
        if (options.contains("headers") && options.at("headers").is_object() &&
            !out.contains("headers")) {
            out["headers"] = options.at("headers");
        }
        out.erase("options");
    }

    if (out.contains("models") && out.at("models").is_object()) {
        nlohmann::json models = nlohmann::json::object();
        for (const auto& [id, model] : out.at("models").items()) {
            nlohmann::json normalized = model.is_object() ? model : nlohmann::json::object();
            if (normalized.contains("limit") && normalized.at("limit").is_object()) {
                const auto& limit = normalized.at("limit");
                if (limit.contains("context") && limit.at("context").is_number_integer() &&
                    !normalized.contains("context_window_tokens")) {
                    normalized["context_window_tokens"] = limit.at("context");
                }
                if (limit.contains("output") && limit.at("output").is_number_integer() &&
                    !normalized.contains("max_tokens")) {
                    normalized["max_tokens"] = limit.at("output");
                }
                normalized.erase("limit");
            }
            if (normalized.contains("options")) {
                // opencode spreads a model's `options` into the request body
                // (the AI SDK does the spreading); minimal's equivalent is
                // `parameters`, which the provider builders emit verbatim
                // before the structural fields. Fold the object over with
                // native `parameters` keys winning per-key -- the same
                // "native keys always win" rule the provider-level fields
                // follow. Anything else (a string, an array) is dropped with
                // a trace: a silent ignore turns "unusual but working" into
                // an unexplained routing/sampling failure at request time.
                if (normalized.at("options").is_object()) {
                    nlohmann::json folded = normalized.at("options");
                    if (normalized.contains("parameters") &&
                        normalized.at("parameters").is_object()) {
                        for (const auto& [key, value] : normalized.at("parameters").items()) {
                            folded[key] = value;
                        }
                    }
                    normalized["parameters"] = std::move(folded);
                } else {
                    minimal::common::DefaultLogger().warn(
                        "providers.json: {} model '{}' field \"options\" must be an object and "
                        "was dropped",
                        where, id);
                }
                normalized.erase("options");
            }
            normalized.erase("name");
            models[id] = std::move(normalized);
        }
        out["models"] = std::move(models);
    }
    out.erase("name");  // opencode's display name; core parses entries strictly
    return out;
}


std::optional<std::string> LookupOpencodeKey(const nlohmann::json& auth_doc,
                                             const std::string& provider_name) {
    if (!auth_doc.is_object() || !auth_doc.contains(provider_name)) return std::nullopt;
    const auto& entry = auth_doc.at(provider_name);
    if (!entry.is_object()) return std::nullopt;
    for (const char* key : {"key", "api_key"}) {
        if (entry.contains(key) && entry.at(key).is_string() &&
            !entry.at(key).get<std::string>().empty()) {
            return entry.at(key).get<std::string>();
        }
    }
    return std::nullopt;
}

}  // namespace minimal::opencode_compat
