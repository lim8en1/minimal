#include "minimal/config/providers_config.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/json_config_loader.hpp"

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

// How an entry's api key came to be. Carries where the key came from, never
// the key itself, so it can be logged verbatim: credentials resolve ONCE at
// parse time, so a key written after the server started (e.g. via /connect)
// is invisible to the running process until restart -- the startup line is
// the only trace of that freeze.
struct ApiKeyResolution {
    std::string key;
    std::string origin;  // "" when no key resolved and none was expected
    bool auth_file_opt_in = false;  // the entry set "credentials": "auth-file"
};

std::optional<std::string> LookupCredentialsFile(const std::string& provider_name,
                                                 std::filesystem::path* found_in);

ApiKeyResolution ResolveApiKey(const nlohmann::json& provider, const std::string& where,
                               const std::string& provider_name) {
    const bool has_key = provider.contains("api_key") && provider.at("api_key").is_string() &&
                         !provider.at("api_key").get<std::string>().empty();
    const bool has_env = provider.contains("api_key_env") &&
                         provider.at("api_key_env").is_string() &&
                         !provider.at("api_key_env").get<std::string>().empty();
    if (has_key && has_env) {
        throw std::runtime_error("providers.json: " + where + " sets both api_key and api_key_env");
    }
    if (has_key) {
        return {provider.at("api_key").get<std::string>(), "inline api_key"};
    }
    if (!has_env) {
        // No inline key, no env: the credentials file is the last link -- but
        // ONLY when this entry explicitly opts in with `"credentials":
        // "auth-file"`. Without that gate, the fallback decouples "who names
        // the request URL" from "who holds the secret": a providers.json entry
        // pasted from an untrusted source, redirecting `url`/`options.baseURL`
        // to an attacker's host and supplying no key of its own, would
        // otherwise silently pull the operator's REAL key out of the shared
        // auth store (this file, then opencode's own) and send it wherever
        // `url` points -- on the first request, with nothing in providers.json
        // that looks like a credential at all. Opt-in makes using the shared
        // store a deliberate, visible act instead of an implicit default;
        // without it, an entry with neither an inline key nor api_key_env
        // resolves empty, same as before this fallback existed.
        const bool opt_in = provider.contains("credentials") &&
                            provider.at("credentials").is_string() &&
                            provider.at("credentials").get<std::string>() == "auth-file";
        ApiKeyResolution resolved;
        resolved.auth_file_opt_in = opt_in;
        if (!opt_in) return resolved;
        std::filesystem::path found_in;
        if (auto key = LookupCredentialsFile(provider_name, &found_in)) {
            resolved.key = std::move(*key);
            resolved.origin = "auth-file (" + found_in.string() + ")";
        } else {
            // The explicit miss state: distinguishable from "configured no
            // credential" on the entry, and the input to the startup warning.
            resolved.origin = "auth-file (no key found)";
        }
        return resolved;
    }

    const std::string name = provider.at("api_key_env").get<std::string>();
    const auto value = common::LookupEnv(name);
    if (!value) {
        throw std::runtime_error("providers.json: " + where + " names api_key_env '" + name +
                                 "' which is unset");
    }
    return {*value, "api_key_env '" + name + "'"};
}

// Last link of the api-key chain: a credentials file. {"<provider>": {"key":
// "..."}}, read from <data dir>/auth.json first and then opencode's own
// credentials (~/.local/share/opencode/auth.json), so an existing opencode
// install's keys are honored zero-copy. Missing files are the normal state;
// malformed ones warn and yield nothing -- a local server needs no key, and a
// cloud provider fails its own auth with a clear 401 rather than blocking
// startup over a file that may belong to a different provider.
std::optional<std::string> LookupKeyInAuthJson(const nlohmann::json& doc,
                                               const std::string& provider_name) {
    if (!doc.is_object() || !doc.contains(provider_name)) return std::nullopt;
    const auto& entry = doc.at(provider_name);
    if (!entry.is_object()) return std::nullopt;
    for (const char* key : {"key", "api_key"}) {
        if (entry.contains(key) && entry.at(key).is_string() &&
            !entry.at(key).get<std::string>().empty()) {
            return entry.at(key).get<std::string>();
        }
    }
    return std::nullopt;
}

std::optional<std::string> LookupAuthFile(const std::filesystem::path& path,
                                          const std::string& provider_name) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    common::ReadFileResult file = common::ReadFile(path);
    if (!file.ok()) {
        if (file.status != common::ReadFileResult::Status::kNotFound) {
            common::DefaultLogger().warn("credentials file ignored: {}", file.error);
        }
        return std::nullopt;
    }
    try {
        return LookupKeyInAuthJson(nlohmann::json::parse(file.contents), provider_name);
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("credentials file {} is malformed and was ignored: {}",
                                     path.string(), error.what());
        return std::nullopt;
    }
}

std::optional<std::string> LookupCredentialsFile(const std::string& provider_name,
                                                 std::filesystem::path* found_in) {
    const auto auth_path = common::DataDir() / "auth.json";
    if (auto key = LookupAuthFile(auth_path, provider_name)) {
        if (found_in) *found_in = auth_path;
        return key;
    }
    // opencode-compat: its own auth store. Windows uses APPDATA, POSIX the
    // XDG-ish path opencode documents.
    if (const auto home = common::LookupEnv("HOME")) {
        const auto oc_path = std::filesystem::path(*home) / ".local" / "share" / "opencode" /
                             "auth.json";
        if (auto key = LookupAuthFile(oc_path, provider_name)) {
            if (found_in) *found_in = oc_path;
            return key;
        }
    }
    return std::nullopt;
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

// The model's value if it has one, else the provider's. A key that is present
// but not an integer is a NAMED error, not a silent truncation: is_number()
// was true for 4096.7, and get<int> happily truncated it -- a typo'd max_tokens
// became a different, valid-looking limit with no message anywhere.
// settings_config.cpp applies the same integer-only rule.
template <typename T>
std::optional<T> InheritNumber(const nlohmann::json& provider, const nlohmann::json& model,
                               const char* key, const std::string& where) {
    for (const auto* node : {&model, &provider}) {
        if (!node->contains(key)) continue;
        const nlohmann::json& value = node->at(key);
        if (!value.is_number_integer()) {
            throw std::runtime_error("providers.json: " + where + " field \"" + key +
                                     "\" must be an integer, got " + value.dump());
        }
        // Both callers are limits (an output cap, a context window): zero or a
        // negative is never meant, and a window of 0 would make the percent
        // compaction trigger fire on every turn.
        if (value.template get<std::int64_t>() < 1) {
            throw std::runtime_error("providers.json: " + where + " field \"" + key +
                                     "\" must be at least 1, got " + value.dump());
        }
        return value.template get<T>();
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> OwnNumber(const nlohmann::json& node, const char* key) {
    if (node.contains(key) && node.at(key).is_number()) return node.at(key).get<T>();
    return std::nullopt;
}



// --- variants parsing --------------------------------------------------------

// The `variants` key on a model or provider, either spelling: an ARRAY of
// effort names (parameters derived per backend at synthesis time) or an OBJECT
// of named full bodies. Anything else is a named parse error -- this file has
// always refused typos where the mistake is.
std::vector<ModelVariant> ParseVariants(const nlohmann::json& node,
                                        const std::string& /*backend*/,
                                        const std::string& where) {
    std::vector<ModelVariant> out;
    if (node.is_array()) {
        for (const auto& item : node) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::runtime_error(
                    "providers.json: " + where +
                    " \"variants\" must be an array of effort names or an object of named "
                    "variant bodies; got a non-string array element " + item.dump());
            }
            const std::string name = item.get<std::string>();
            if (!IsKnownEffortName(name)) {
                throw std::runtime_error(
                    "providers.json: " + where + " names variant '" + name +
                    "', which is not a known effort (known: low, medium, high, xhigh, max, "
                    "minimal, none)");
            }
            // No parameters here: MakeVariantResolved derives the wire map from
            // the name at sibling-build time, where the base model's resolved
            // max_tokens is available for the anthropic budget clamp. This
            // function only validates and records the name.
            out.push_back(ModelVariant{name, nlohmann::json::object(), std::nullopt,
                                       std::nullopt});
        }
        return out;
    }
    if (node.is_object()) {
        for (const auto& [name, body] : node.items()) {
            if (name.empty() || name.find('@') != std::string::npos) {
                throw std::runtime_error("providers.json: " + where + " names variant '" +
                                         name +
                                         "' -- a variant name must be non-empty and must not "
                                         "contain '@'");
            }
            if (!body.is_object()) {
                throw std::runtime_error("providers.json: " + where + " variant '" + name +
                                         "' must be an object");
            }
            ModelVariant variant;
            variant.name = name;
            if (body.contains("parameters")) {
                if (!body.at("parameters").is_object()) {
                    throw std::runtime_error("providers.json: " + where + " variant '" + name +
                                             "' field \"parameters\" must be an object");
                }
                variant.parameters = body.at("parameters");
            }
            for (const char* key : {"max_tokens", "context_window_tokens"}) {
                if (body.contains(key)) {
                    const nlohmann::json& value = body.at(key);
                    if (!value.is_number_integer() || value <= 0) {
                        throw std::runtime_error("providers.json: " + where + " variant '" +
                                                 name + "' field \"" + key +
                                                 "\" must be a positive integer, got " +
                                                 value.dump());
                    }
                    if (std::string_view(key) == "max_tokens") {
                        variant.max_tokens = value.get<int>();
                    } else {
                        variant.context_window_tokens = value.get<std::int64_t>();
                    }
                }
            }
            for (const auto& [key, unused] : body.items()) {
                (void)unused;
                if (key != "parameters" && key != "max_tokens" &&
                    key != "context_window_tokens") {
                    throw std::runtime_error("providers.json: " + where + " variant '" + name +
                                             "' has unknown field \"" + key +
                                             "\" (known: parameters, max_tokens, "
                                             "context_window_tokens)");
                }
            }
            out.push_back(std::move(variant));
        }
        return out;
    }
    throw std::runtime_error(
        "providers.json: " + where +
        " \"variants\" must be an array of effort names or an object of named variant "
        "bodies, got " + node.dump());
}

// The model's own variants, else the provider's (per-model beats per-provider,
// the same precedence InheritParameters applies at the parameter layer). Both
// absent = empty = "no declared variants" (discovery then applies the entry's
// recipe, which itself defaults to the model's autodetected effort ladder --
// see PublishLocked).
std::vector<ModelVariant> InheritVariants(const nlohmann::json& provider,
                                          const nlohmann::json& model,
                                          const std::string& backend,
                                          const std::string& where) {
    if (model.contains("variants")) {
        return ParseVariants(model.at("variants"), backend, where);
    }
    if (provider.contains("variants")) {
        return ParseVariants(provider.at("variants"), backend, where + " (provider)");
    }
    return {};
}

// Same precedence InheritVariants applies, but answering only "was a
// `variants` key present at all" -- including one that parses to an empty
// list, which InheritVariants' return value alone cannot distinguish from
// "nothing declared".
bool HasDeclaredVariants(const nlohmann::json& provider, const nlohmann::json& model) {
    return model.contains("variants") || provider.contains("variants");
}


//
//   options.baseURL -> url        (only when url absent)
//   options.apiKey  -> api_key    (only when neither api_key nor api_key_env)
//   options.headers / headers -> connection headers (additive, both shapes)
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
//   blacklist / whitelist -> the picker filters (parsed into the entry below).
nlohmann::json NormalizeOpencodeProvider(const nlohmann::json& provider,
                                         const std::string& where) {
    if (!provider.is_object()) return provider;
    nlohmann::json out = provider;

    if (out.contains("npm") && out.at("npm").is_string()) {
        const std::string npm = out.at("npm").get<std::string>();
        if (!out.contains("backend")) {
            if (npm == "@ai-sdk/openai-compatible") {
                out["backend"] = "openai";
            } else {
                common::DefaultLogger().warn(
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
            !out.contains("api_key") && !out.contains("api_key_env")) {
            out["api_key"] = options.at("apiKey");
        }
        if (options.contains("headers") && options.at("headers").is_object() &&
            !out.contains("headers")) {
            out["headers"] = options.at("headers");
        }
        out.erase("options");
    }

    // Headers: either shape normalizes to a flat array of [name, value] pairs.
    if (out.contains("headers") && out.at("headers").is_object()) {
        nlohmann::json flat = nlohmann::json::array();
        for (const auto& [name, value] : out.at("headers").items()) {
            if (value.is_string()) {
                flat.push_back(nlohmann::json::array({name, value}));
            } else if (value.is_number_integer() || value.is_number_float() ||
                       value.is_boolean()) {
                // Coerce scalars, don't drop them: opencode-shaped configs
                // carry numeric/boolean header values, and a silent drop
                // turns "unusual but working" into a discovery or auth
                // failure with no trace in any log.
                flat.push_back(
                    nlohmann::json::array({name, value.is_boolean()
                                                    ? std::string(value.get<bool>() ? "true" : "false")
                                                    : value.dump()}));
            } else {
                common::DefaultLogger().warn(
                    "providers.json: header '{}' has a non-scalar value and was dropped", name);
            }
        }
        out["headers"] = std::move(flat);
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
                    common::DefaultLogger().warn(
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
    return out;
}

}  // namespace

// --- effort variants ---------------------------------------------------------

VariantParamsHook MakeVariantParamsHook(const llm::VariantParamsRegistry* registry) {
    if (registry == nullptr) return {};
    return [registry](const std::string& kind, const std::string& effort_word,
                      std::optional<int> max_tokens) -> std::optional<std::pair<nlohmann::json, bool>> {
        auto* provider = registry->Find(kind);
        if (provider == nullptr) return std::nullopt;
        return provider->DeriveParameters(effort_word, max_tokens);
    };
}

// The known effort ladder, sorted. Names follow the union of the live catalogs
// (OpenRouter's `reasoning.supported_efforts`, models.dev's `reasoning_options`):
// the six words between them, with "none" meaning reasoning off where the
// backend supports a disabled state.
const std::vector<std::string>& KnownEffortNames() {
    static const std::vector<std::string> names = {"high", "low", "max", "medium",
                                                   "minimal", "none", "xhigh"};
    return names;
}

bool IsKnownEffortName(const std::string& name) {
    const auto& names = KnownEffortNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::pair<nlohmann::json, bool> VariantParametersFor(const std::string& backend,
                                                     const std::string& effort,
                                                     std::optional<int> max_tokens,
                                                     const VariantParamsHook& hook) {
    // Backend-agnostic: "xhigh"/"max" top out whichever ladder the backend
    // speaks. Computed once, before dispatch, so it cannot drift between a
    // plugin-registered mapping and the generic fallback below.
    const std::string effort_word = (effort == "xhigh" || effort == "max") ? "high" : effort;

    if (hook) {
        if (auto derived = hook(backend, effort_word, max_tokens)) return *derived;
        // hook set but returned nullopt: `backend` has no plugin-registered
        // mapping (unregistered kind, or the kind's plugin isn't loaded) --
        // fall through to the generic shape below, exactly as any backend
        // that never had one already does.
    }

    // Off, where the generic shape has no meaningful "off" spelling every
    // backend accepts -- send the toggle word instead (a toggle-only model
    // always thinks); otherwise the name passes through verbatim.
    nlohmann::json out;
    out["reasoning_effort"] = (effort_word == "none") ? "minimal" : effort_word;
    return {out, false};
}

std::pair<std::string, std::string> SplitVariantSuffix(const std::string& model_id) {
    const auto at = model_id.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == model_id.size()) {
        return {model_id, ""};
    }
    return {model_id.substr(0, at), model_id.substr(at + 1)};
}

// The sibling ResolvedModel for `base` at variant `variant`: same connection
// and WIRE model id, the variant's parameters overlaid onto the base's (variant
// wins per key, the same rule preset-extras apply over model parameters),
// variant-scoped cap/window when the variant carries them.
//
// A list-form variant (parameters empty, name a known effort) has its wire
// parameters derived HERE, against the base's resolved output cap -- the one
// place both the backend kind and the inherited max_tokens are known, whether
// the base came from the static parse or discovery. ParseVariants only
// validates the name; this owns the derivation, so the clamp rule cannot drift
// between the two birthplaces of a variant.
ResolvedModel EnrichFromDiscovery(const ResolvedModel& static_model,
                                  const llm::DiscoveredModel& discovered) {
    ResolvedModel copy = static_model;
    if (!copy.context_window_tokens && discovered.context_window_tokens) {
        copy.context_window_tokens = discovered.context_window_tokens;
    }
    if (!copy.max_tokens && discovered.max_tokens) {
        copy.max_tokens = discovered.max_tokens;
    }
    return copy;
}

ResolvedModel MakeVariantResolved(const ResolvedModel& base, const ModelVariant& variant,
                                  const VariantParamsHook& variant_params_hook = {}) {
    ResolvedModel sibling = base;
    sibling.ref = ModelRef{base.ref.provider, base.ref.model + "@" + variant.name};
    // THE WIRE ID STAYS THE BASE'S. agent_loop sends provider.model_name
    // verbatim; the @suffix is addressing only and must never reach the wire.
    sibling.provider.model_name = base.provider.model_name;
    nlohmann::json parameters = variant.parameters;
    if (parameters.empty() && IsKnownEffortName(variant.name)) {
        const auto [derived, clamped] = VariantParametersFor(base.backend, variant.name,
                                                             base.max_tokens, variant_params_hook);
        parameters = derived;
        if (clamped) {
            common::DefaultLogger().warn(
                "providers.json: variant '@{}' of model '{}' had a wire parameter clamped to fit "
                "max_tokens {}",
                variant.name, base.ref.model, *base.max_tokens);
        }
    }
    for (const auto& [key, value] : parameters.items()) {
        sibling.parameters[key] = value;
    }
    if (variant.max_tokens) sibling.max_tokens = variant.max_tokens;
    if (variant.context_window_tokens) sibling.context_window_tokens = *variant.context_window_tokens;
    return sibling;
}


namespace {

// The overlay ResolvedModel for one discovered model: the entry's connection
// and provider-level defaults, the backend's id, window, and output cap.
// Touches neither ImmutableCatalog nor DiscoveryCacheState directly -- a pure
// function of its two parameters despite living beside methods that hold
// cache_.mutex_ -- so it needs no ProvidersConfig membership at all.
ResolvedModel BuildDiscoveredLocked(const ProviderEntry& entry,
                                    const llm::DiscoveredModel& discovered) {
    // The backend knows the model id it serves; the entry supplies the
    // connection and the provider-level defaults, inherited exactly as a
    // static model would inherit them. The backend's context window and
    // output cap win over the provider-level defaults -- the same precedence
    // InheritNumber gives a model's own value over the provider's.
    ResolvedModel resolved;
    resolved.ref = ModelRef{entry.name, discovered.id};
    resolved.backend = entry.backend;
    resolved.provider = entry.connection;
    resolved.provider.model_name = discovered.id;
    resolved.parameters = entry.parameters;
    resolved.max_tokens = discovered.max_tokens ? discovered.max_tokens : entry.max_tokens;
    resolved.context_window_tokens =
        discovered.context_window_tokens ? discovered.context_window_tokens
                                         : entry.context_window_tokens;
    return resolved;
}

// OpenRouter app-attribution opt-in ("openrouter_app_id": true). Every request
// on the entry -- chat and /models discovery alike -- carries the project's
// identity, so usage is credited to it on OpenRouter's app leaderboard: the
// same opt-in opencode bakes into its openrouter provider (HTTP-Referer +
// X-Title). The headers are SYNTHESIZED at parse time; an explicit "headers"
// entry of the same name wins (the file's native-keys-always-win rule), so an
// operator can rebrand or point the referer elsewhere without losing the
// other half. Backend-agnostic: any backend kind simply carries two extra
// headers it ignores.
constexpr const char* kAppAttributionTitle = "minimal";
constexpr const char* kAppAttributionReferer = "https://github.com/lim8en1/minimal";

void ApplyAppAttribution(const nlohmann::json& provider, llm::ProviderConfig& connection,
                         const std::string& where) {
    if (!provider.contains("openrouter_app_id")) return;
    const auto& flag = provider.at("openrouter_app_id");
    if (!flag.is_boolean()) {
        throw std::runtime_error("providers.json: " + where +
                                 " sets \"openrouter_app_id\" which must be a boolean");
    }
    if (!flag.get<bool>()) return;
    const std::pair<const char*, const char*> synthesized[] = {
        {"X-Title", kAppAttributionTitle},
        {"HTTP-Referer", kAppAttributionReferer},
    };
    for (const auto& [name, value] : synthesized) {
        const auto existing =
            std::find_if(connection.headers.begin(), connection.headers.end(),
                         [&](const auto& header) { return header.first == name; });
        if (existing != connection.headers.end()) continue;
        connection.headers.emplace_back(name, value);
    }
}

}  // namespace

ProvidersConfig& ProvidersConfig::operator=(ProvidersConfig&& other) noexcept {
    if (this == &other) return *this;  // would lock the same mutex twice
    std::lock_guard<std::mutex> a(cache_.mutex_);
    std::lock_guard<std::mutex> b(other.cache_.mutex_);
    catalog_.models_ = std::move(other.catalog_.models_);
    catalog_.providers_ = std::move(other.catalog_.providers_);
    cache_.gates_ = std::move(other.cache_.gates_);
    cache_.hook_ = std::move(other.cache_.hook_);
    cache_.metadata_hook_ = std::move(other.cache_.metadata_hook_);
    cache_.variant_params_hook_ = std::move(other.cache_.variant_params_hook_);
    cache_.metadata_variants_ = std::move(other.cache_.metadata_variants_);
    cache_.metadata_tombstones_ = std::move(other.cache_.metadata_tombstones_);
    cache_.generation_ = other.cache_.generation_;
    cache_.discovery_ = std::move(other.cache_.discovery_);
    cache_.overlay_ = std::move(other.cache_.overlay_);
    return *this;
}

void ProvidersConfig::Add(const ModelRef& ref, ResolvedModel resolved) {
    catalog_.models_.insert_or_assign(ref.str(),
                                      std::make_shared<const ResolvedModel>(std::move(resolved)));
}

void ProvidersConfig::AddProvider(ProviderEntry entry) {
    catalog_.providers_.insert_or_assign(entry.name, std::move(entry));
}

std::vector<std::string> ProvidersConfig::VariantsOf(const ModelRef& ref) const {
    // A ref that already names a variant answers with nothing -- the base is
    // the thing that carries a variant list; `@x@y` is not a shape anyone can
    // have declared.
    const auto [base_id, variant_name] = SplitVariantSuffix(ref.model);
    if (!variant_name.empty()) return {};
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    return VariantsOfLocked(ref.provider, base_id);
}

// The variant recipe lookup under cache_.mutex_ -- the body of VariantsOf,
// shared with ModelRefs' variant-refs union (which needs the same
// static-catalog-beats-discovery-cache precedence while already holding the
// mutex). See VariantsOf's public contract for the rules.
std::vector<std::string> ProvidersConfig::VariantsOfLocked(const std::string& provider_name,
                                                           const std::string& base_id) const {
    std::vector<std::string> out;
    const std::string key = provider_name + "/" + base_id;

    // Static base: parse-time synthesis put every declared sibling in the
    // catalog; list the refs that share the base prefix. A static base that
    // declared its own variants answers from here alone -- explicit beats
    // autodetection. But a static base that declared NONE on a discover entry
    // (the common shape: a static twin supplying parameters the discovery
    // listing cannot express) still carries the autodetected ladder, so an
    // empty answer falls through to the discovery-cache lookup below.
    const std::string prefix = key + "@";
    if (const auto base_it = catalog_.models_.find(key); base_it != catalog_.models_.end()) {
        for (const auto& [candidate, unused] : catalog_.models_) {
            (void)unused;
            if (candidate.rfind(prefix, 0) == 0) out.push_back(candidate.substr(prefix.size()));
        }
        if (!out.empty()) return out;
        // An explicit `variants: []` (or `{}`) is an operator's deliberate
        // opt-out, not "nothing to say" -- honor it here rather than falling
        // through to the discovery cache's autodetected ladder below, which
        // would silently re-enable the variants the entry tried to suppress.
        if (base_it->second->variants_declared) return out;
    }
    // Discovered base (cold cache contributes nothing -- a listing, never a
    // fetch trigger, the same rule ModelRefs applies), or a static base whose
    // discovery twin advertises a ladder: the recipe recorded at publish.
    // NB: cold-cache-here does NOT consult metadata either -- VariantsOf is a
    // listing, and triggering enrichment I/O from a listing would make the
    // picker latency-bound. The ladder lands here the first time the base (or
    // one of its variants) actually resolves.
    if (const auto cache_it = cache_.discovery_.find(provider_name);
        cache_it != cache_.discovery_.end() && cache_it->second.ok) {
        const auto variants_it = cache_it->second.variants_by_model.find(base_id);
        if (variants_it != cache_it->second.variants_by_model.end()) {
            for (const auto& variant : variants_it->second) out.push_back(variant.name);
            return out;
        }
        // Discovery is ok but this base is a static-only model its listing
        // does not name: fall through -- the metadata recipe (if the base was
        // enriched) still applies.
    }
    // The metadata-enrichment recipe, when the base has been enriched
    // already. Not a consultation trigger -- see the NB above.
    if (const auto metadata_it = cache_.metadata_variants_.find(key);
        metadata_it != cache_.metadata_variants_.end()) {
        for (const auto& variant : metadata_it->second) out.push_back(variant.name);
    }
    return out;
}

const ProviderEntry* ProvidersConfig::FindProviderLocked(const std::string& name) const {
    const auto it = catalog_.providers_.find(name);
    return it == catalog_.providers_.end() ? nullptr : &it->second;
}

void ProvidersConfig::SetDiscoveryHook(DiscoveryHook hook) {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    cache_.hook_ = std::move(hook);
    // A discovery attempted under the previous hook -- typically none at all,
    // when the startup sanity check resolved against this catalog before
    // plugins' catalogs were wired in -- is stale: refetch it at next resolve
    // rather than serving its negative-cached "no discovery hook set".
    ++cache_.generation_;
}

void ProvidersConfig::SetDiscoveryRetryBackoff(std::chrono::milliseconds backoff) {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    cache_.discovery_retry_backoff_ = backoff;
}

void ProvidersConfig::SetMetadataHook(MetadataHook hook) {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    cache_.metadata_hook_ = std::move(hook);
}

void ProvidersConfig::SetVariantParamsHook(VariantParamsHook hook) {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    cache_.variant_params_hook_ = std::move(hook);
}

std::shared_ptr<const ResolvedModel> ProvidersConfig::Resolve(const ModelRef& ref) const {
    const std::string key = ref.str();
    std::unique_lock<std::mutex> lock(cache_.mutex_);

    // A `@variant` suffix is checked FIRST: the ref does not exist as a static
    // or discovered id, and the suffix must be validated against the base
    // model's declared variants, not treated as a (missing) raw id.
    if (const auto [base_id, variant_name] = SplitVariantSuffix(ref.model);
        !variant_name.empty()) {
        const std::string base_key = ref.provider + "/" + base_id;
        // The base may be a static entry or a discovered one; Resolve of the
        // stripped ref answers that (and triggers the lazy fetch, which is
        // what makes a discovered base's variants resolvable on first use).
        if (const ProviderEntry* entry = FindProviderLocked(ref.provider);
            entry != nullptr) {
            if (auto resolved = ResolveVariant(*entry, base_id, variant_name, key, lock)) {
                return resolved;
            }
        }
        // Fall through to the plain-id paths ONLY when the base id itself
        // contains an '@' that SplitVariantSuffix would have misread. Re-split
        // is unnecessary: if the base id exists as a literal (no live catalog
        // serves '@', but a hand-written one may), honor it verbatim.
        if (catalog_.models_.count(key) != 0) {
            return catalog_.models_.at(key);
        }
        // Not a declared variant, not a literal '@' id: a miss, reported by
        // the caller as "unknown model" -- never a synthesized guess.
        return nullptr;
    }

    if (const auto it = catalog_.models_.find(key); it != catalog_.models_.end()) {
        if (it->second->context_window_tokens && it->second->max_tokens) return it->second;
        // A static entry usually resolves as parsed. Under a discover-entry, a
        // model whose context_window_tokens or max_tokens is absent can still
        // be enriched from the catalog -- but only by handing back an overlay
        // COPY: the parsed node must never be mutated after a snapshot of it
        // has escaped.
        if (const auto enriched = cache_.overlay_.find(key); enriched != cache_.overlay_.end()) {
            return enriched->second;
        }
        if (const ProviderEntry* entry = FindProviderLocked(ref.provider);
             entry != nullptr && entry->discover) {
            // The catalog's answer for a model it lists, or nothing when the
            // fetch failed. A STATIC hit degrades to its parsed self either
            // way -- a dead backend must not take a hand-written model down
            // with it (this is the "failed discovery degrades to the entry's
            // static models" rule; before the output cap, a windowed static
            // model never reached discovery at all, so the degradation was
            // only ever observable for window-less ones).
            if (auto resolved = ResolveDiscovered(*entry, ref.model, key, lock)) {
                return resolved;
            }
            // Discovery could not fill the gaps (failed fetch, or the model is
            // a static twin its listing does not name): the metadata layer
            // gets its turn before the parsed self is handed back.
            if (entry->metadata) {
                EnrichFromMetadata(*entry, ref.model, key, lock);
                if (const auto enriched = cache_.overlay_.find(key);
                    enriched != cache_.overlay_.end()) {
                    return enriched->second;
                }
            }
            return it->second;
        }
        // Metadata enrichment (docs/EXTENSIBILITY.md §2.13): a non-discover
        // entry's static model missing its window/cap consults the metadata
        // registry -- the auto default, declined per entry with
        // "metadata": false. Same overlay-copy rule as above.
        if (const ProviderEntry* entry = FindProviderLocked(ref.provider);
            entry != nullptr && entry->metadata) {
            EnrichFromMetadata(*entry, ref.model, key, lock);
            if (const auto enriched = cache_.overlay_.find(key); enriched != cache_.overlay_.end()) {
                return enriched->second;
            }
        }
        return it->second;
    }

    // A static miss under a discover-entry may still be a model the backend
    // serves. Static entries elsewhere miss as today -- null, caller reports.
    if (const ProviderEntry* entry = FindProviderLocked(ref.provider);
        entry != nullptr && entry->discover) {
        return ResolveDiscovered(*entry, ref.model, key, lock);
    }
    return nullptr;
}

std::shared_ptr<const ResolvedModel> ProvidersConfig::ResolveDiscovered(
    const ProviderEntry& entry, const std::string& model_id, const std::string& ref_key,
    std::unique_lock<std::mutex>& lock) const {
    // The fetch runs holding ONLY this provider's gate: a slow catalog must
    // not stall other config reads -- session creation, listings -- which take
    // cache_.mutex_ alone. Lock order is always gate -> cache_.mutex_, never
    // the reverse: cache_.gates_ is only ever touched for insertion here
    // (under cache_.mutex_) or locked before it.
    lock.unlock();
    DiscoveryCacheState::FetchGate& gate = [&] () -> DiscoveryCacheState::FetchGate& {
        std::lock_guard<std::mutex> l(cache_.mutex_);
        return cache_.gates_.try_emplace(entry.name).first->second;
    }();
    std::lock_guard<std::mutex> gate_lock(gate.mtx);
    lock.lock();

    // Double-check under the gate: another resolver may have fetched while we
    // waited, and its publish may already serve this model.
    if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) return it->second;

    DiscoveryCacheState::DiscoveryCache& cache = cache_.discovery_[entry.name];
    // A failed attempt is retried once its backoff elapsed: the negative cache
    // protects against a fetch per resolve, not against a backend that came up
    // after the first look.
    const bool failed_and_due =
        cache.attempted && !cache.ok &&
        std::chrono::steady_clock::now() - cache.attempted_at >= cache_.discovery_retry_backoff_;
    if (!cache.attempted || cache.generation != cache_.generation_ || failed_and_due) {
        lock.unlock();  // the hook does network I/O -- no config lock held
        const DiscoveryOutcome outcome = FetchOutcome(entry);
        lock.lock();
        // A refresh may have published while we fetched; prefer its result.
        if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) return it->second;
        PublishLocked(entry, outcome, cache);
    }

    if (!cache.ok) return nullptr;

    if (const auto static_it = catalog_.models_.find(ref_key); static_it != catalog_.models_.end()) {
        // A STATIC model under a discover-entry: its own inheritance stands,
        // and the discovered window / output cap fill only the gaps that made
        // Resolve consult discovery at all -- each field independently, since
        // a model with a window but no cap reaches here too.
        const auto discovered = cache.models.find(model_id);
        if (discovered != cache.models.end()) {
            return cache_.overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(
                                                        EnrichFromDiscovery(*static_it->second,
                                                                            discovered->second)))
                .first->second;
        }
        return static_it->second;  // nothing to enrich
    }
    if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) return it->second;
    return nullptr;  // discovery succeeded; the backend does not serve this id
}

std::shared_ptr<const ResolvedModel> ProvidersConfig::ResolveVariant(
    const ProviderEntry& entry, const std::string& base_id, const std::string& variant_name,
    const std::string& ref_key, std::unique_lock<std::mutex>& lock) const {
    // Served-from-overlay first: the sibling is built at most once per ref per
    // process, exactly like every other overlay value (RefreshDiscovery evicts
    // wholesale, so a rebuild after a refresh picks up a changed recipe).
    if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) {
        return it->second;
    }

    const std::string base_key = entry.name + "/" + base_id;

    // The variant must be DECLARED for this base. Static bases declared theirs
    // at parse time -- and their siblings were materialized into the catalog
    // then, so a static variant simply IS a catalog entry; check that first and
    // skip the rebuild entirely.
    if (const auto static_it = catalog_.models_.find(ref_key); static_it != catalog_.models_.end()) {
        return static_it->second;
    }

    // Everything else resolves through the base. Resolve the base WITHOUT the
    // suffix: a static base resolves as parsed (plus the discovery
    // enrichment, EnrichFromDiscovery), a discovered base runs the
    // lazy fetch (ResolveDiscovered unlocks around the hook, per its contract),
    // which is what makes `...@high` on a discover-entry fetch on first use.
    std::shared_ptr<const ResolvedModel> base;
    if (const auto base_it = catalog_.models_.find(base_key); base_it != catalog_.models_.end()) {
        // Enrich the static base from its discovery twin -- the sibling must
        // carry the same window/cap a session on the base would get, or a
        // /think switch silently drops compaction.
        if (entry.discover) {
            if (const auto cache_it = cache_.discovery_.find(entry.name);
                cache_it != cache_.discovery_.end() && cache_it->second.ok) {
                const auto discovered = cache_it->second.models.find(base_id);
                if (discovered != cache_it->second.models.end()) {
                    base = std::make_shared<const ResolvedModel>(
                        EnrichFromDiscovery(*base_it->second, discovered->second));
                }
            }
        }
        if (!base) base = base_it->second;
    } else if (entry.discover) {
        base = ResolveDiscovered(entry, base_id, base_key, lock);
    }
    if (base == nullptr) return nullptr;  // unknown base; caller reports the miss

    // The declared variant for this name. A static base: its recipe lived in
    // providers.json -- but parse-time synthesis put every declared static
    // sibling in the catalog (handled above), so reaching here with a static
    // base means the variant was NOT declared by config. It may still have an
    // AUTODETECTED ladder from metadata enrichment: consult the recorded
    // recipe (EnrichFromMetadata fills it on the base's first resolve; the
    // consult in the static branch below covers a `...@high` ref arriving
    // before any base resolve). A discovered base: the entry's recipe
    // (defaulting to the model's autodetected ladder), with what the
    // catalog's metadata advertised -- PublishLocked already recorded that
    // intersection in the discovery cache's variant list when it materialized
    // the base, so this is a lookup, never a re-derivation.
    std::optional<ModelVariant> declared;
    if (!entry.discover) {
        // A static twin's explicit `variants: []` opt-out (see VariantsOf's
        // identical check) applies here too: reaching this line already means
        // no static sibling matched `variant_name`, so an opted-out base must
        // answer "undeclared" rather than consulting the metadata ladder.
        if (base->variants_declared) return nullptr;
        // A `...@high` ref can arrive before the base itself was ever
        // resolved -- pull the metadata recipe in (no-op when it already
        // exists; the same per-provider-gate consult EnrichFromMetadata owns).
        if (entry.metadata) EnrichFromMetadata(entry, base_id, base_key, lock);
        // The base must be the ENRICHED copy (window/cap/parameters), not the
        // parsed node the overlay also holds -- the sibling carries what a
        // session on the base would get, exactly as ResolveDiscovered's static
        // branch does for the discovery twin.
        if (const auto enriched_it = cache_.overlay_.find(base_key);
            enriched_it != cache_.overlay_.end()) {
            base = enriched_it->second;
        }
        const auto metadata_it = cache_.metadata_variants_.find(base_key);
        if (metadata_it == cache_.metadata_variants_.end()) return nullptr;
        const auto name_it = std::find_if(metadata_it->second.begin(),
                                          metadata_it->second.end(),
                                          [&](const ModelVariant& v) {
                                              return v.name == variant_name;
                                          });
        if (name_it == metadata_it->second.end()) return nullptr;
        return cache_
            .overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(MakeVariantResolved(
                                           *base, *name_it, cache_.variant_params_hook_)))
            .first->second;
    }
    // A static twin's explicit `variants: []` opt-out (see VariantsOf's
    // identical check) applies here too: reaching this line already means no
    // static sibling matched `variant_name`, so an opted-out base must answer
    // "undeclared" rather than falling through to the autodetected ladder.
    if (base->variants_declared) return nullptr;
    if (const auto cache_it = cache_.discovery_.find(entry.name);
        cache_it != cache_.discovery_.end() && cache_it->second.ok) {
        const auto variants_it = cache_it->second.variants_by_model.find(base_id);
        if (variants_it != cache_it->second.variants_by_model.end()) {
            const auto name_it = std::find_if(variants_it->second.begin(),
                                              variants_it->second.end(),
                                              [&](const ModelVariant& v) {
                                                  return v.name == variant_name;
                                              });
            if (name_it != variants_it->second.end()) declared = *name_it;
        }
    }
    if (!declared) return nullptr;  // undeclared (or the model advertises no efforts)

    return cache_
        .overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(MakeVariantResolved(
                                       *base, *declared, cache_.variant_params_hook_)))
        .first->second;
}

void ProvidersConfig::EnrichFromMetadata(const ProviderEntry& entry, const std::string& model_id,
                                         const std::string& ref_key,
                                         std::unique_lock<std::mutex>& lock) const {
    // cache_.mutex_ is HELD on entry (via `lock`): the hook snapshot and the
    // already-enriched checks below read mutex-guarded state directly, never
    // re-locking -- a second acquisition of a non-recursive mutex this caller
    // already holds is a deadlock, not a scope.
    MetadataHook hook = cache_.metadata_hook_;
    // Already enriched (or already answered "no answer"): the overlay or
    // tombstone IS the result; a second consult would repeat the Lookup on
    // every resolve of the same model.
    if (cache_.overlay_.count(ref_key) != 0 || cache_.metadata_tombstones_.count(ref_key) != 0) {
        return;
    }
    if (!hook) return;

    // The hook may do I/O (the modelsdev plugin reads its cache; a refresh
    // may hit the network). Hold ONLY this provider's gate across it -- the
    // same discipline ResolveDiscovered uses, for the same reason: a slow
    // catalog must not stall session creation or listings, which take
    // cache_.mutex_ alone. Lock order is always gate -> mutex_, never the
    // reverse.
    lock.unlock();
    DiscoveryCacheState::FetchGate& gate = [&] () -> DiscoveryCacheState::FetchGate& {
        std::lock_guard<std::mutex> held(cache_.mutex_);
        return cache_.gates_.try_emplace(entry.name).first->second;
    }();
    std::lock_guard<std::mutex> gate_lock(gate.mtx);
    // Deadlock note, learned the hard way: the gate lambda above MUST be the
    // only place this function touches cache_.mutex_ while `lock` is
    // unlocked -- it is fine here because `lock` is NOT holding the mutex at
    // that moment, and the re-lock below happens after the lambda's guard has
    // released. Never take cache_.mutex_ again while `lock` holds it.

    llm::MetadataQuery query;
    query.model_id = model_id;
    query.base_url = entry.connection.base_url;
    query.backend = entry.backend;
    query.provider_name = entry.name;
    query.metadata_provider = entry.metadata_provider;
    std::optional<llm::DiscoveredModel> answer;
    try {
        answer = hook(query, entry.metadata_match);
    } catch (...) {
        // The hook's own contract is to degrade, not throw; this is
        // defense-in-depth, and it degrades the same way a nullopt would.
        answer = std::nullopt;
    }

    lock.lock();
    // A refresh may have published while we consulted; prefer its result.
    if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) return;
    // The model was parsed away? It cannot be -- catalog_ is parse-time only
    // and ProvidersConfig has no removal path. The double-check under the gate
    // still re-verifies the tombstone in case a concurrent resolver enriched
    // terminal, so re-consulting never happens either way.
    if (cache_.metadata_tombstones_.count(ref_key) != 0) return;

    if (!answer.has_value()) {
        cache_.metadata_tombstones_.emplace(ref_key, true);
        return;
    }

    const auto static_it = catalog_.models_.find(ref_key);
    if (static_it == catalog_.models_.end()) {
        // Unreachable by construction (Lookup is keyed by an existing id) --
        // recorded as a miss rather than trusted.
        cache_.metadata_tombstones_.emplace(ref_key, true);
        return;
    }
    // Gap-fill only: user-declared fields always win (EnrichFromDiscovery's
    // rule), and the overlay copy never mutates the parsed node.
    ResolvedModel enriched = EnrichFromDiscovery(*static_it->second, *answer);
    // The autodetected ladder from the metadata answer, for models that
    // declare no variants of their own -- the same recipe PublishLocked
    // computes for discovered models. Unknown effort names warn-and-skip so
    // an autodetected selector never resolves to a request identical to the
    // base model with no trace why.
    std::vector<ModelVariant> ladder;
    if (!enriched.variants_declared) {
        for (const auto& effort : llm::AdvertisedEfforts(answer->metadata)) {
            if (!IsKnownEffortName(effort)) {
                common::DefaultLogger().warn(
                    "providers.json: model '{}' advertises an unknown effort '{}' via metadata "
                    "enrichment; skipping that variant (known: high, low, max, medium, minimal, "
                    "none, xhigh)",
                    model_id, effort);
                continue;
            }
            ladder.push_back(ModelVariant{effort, {}, std::nullopt, std::nullopt});
        }
    }
    cache_.metadata_variants_.emplace(ref_key, std::move(ladder));
    cache_.overlay_.emplace(ref_key, std::make_shared<const ResolvedModel>(std::move(enriched)));
}

DiscoveryOutcome ProvidersConfig::FetchOutcome(const ProviderEntry& entry) const {
    DiscoveryOutcome outcome;
    // Snapshot the hook under cache_.mutex_ (lock order gate -> mutex_, per
    // DiscoveryCacheState) rather than reading it bare: SetDiscoveryHook
    // writes it under that mutex, and a bare read of a std::function mid-assign
    // is UB, however unlikely the overlap. The copy is released before the
    // hook runs -- the network I/O must not hold any config lock.
    DiscoveryHook hook;
    MetadataHook metadata_hook;
    {
        std::lock_guard<std::mutex> lock(cache_.mutex_);
        hook = cache_.hook_;
        metadata_hook = cache_.metadata_hook_;
    }
    if (!hook) {
        // Uninjected (a test or an unhosted config): report rather than treat
        // as success-with-zero-models, which would read as "backend serves
        // nothing".
        outcome.catalog_present = true;
        outcome.error = "no discovery hook set";
        return outcome;
    }
    outcome = hook(entry.backend, entry.connection);
    // Metadata defaults (docs/EXTENSIBILITY.md §2.13): the discovery listing
    // wins on ids and on anything IT reports, and the metadata layer fills
    // only what it leaves unset -- a listed model with no context window (the
    // vLLM gap this seam exists for) gets models.dev's window without a line
    // of config. Consulted HERE, inside the same per-provider gate the fetch
    // holds, so the fill is serialized with the fetch that feeds it and a
    // refreshed listing re-fills from the current metadata answer. A model
    // that declares its own `variants` keeps them: the ladder below is
    // skipped for any model whose metadata already advertises efforts, and
    // PublishLocked's declared-recipe precedence is untouched.
    if (outcome.ok && entry.metadata && metadata_hook && !outcome.models.empty()) {
        for (auto& model : outcome.models) {
            if (model.context_window_tokens.has_value() && model.max_tokens.has_value()) continue;
            llm::MetadataQuery query;
            query.model_id = model.id;
            query.base_url = entry.connection.base_url;
            query.backend = entry.backend;
            query.provider_name = entry.name;
            query.metadata_provider = entry.metadata_provider;
            try {
                if (auto fill = metadata_hook(query, entry.metadata_match);
                    fill.has_value()) {
                    if (!model.context_window_tokens.has_value() &&
                        fill->context_window_tokens.has_value()) {
                        model.context_window_tokens = fill->context_window_tokens;
                    }
                    if (!model.max_tokens.has_value() && fill->max_tokens.has_value()) {
                        model.max_tokens = fill->max_tokens;
                    }
                    // No ladder of its own from the listing: the metadata
                    // answer's `reasoning_options` IS the advertised effort
                    // set -- merged into the metadata the variant recipe
                    // reads below, never overriding what the listing said.
                    if (model.metadata.is_object() && !model.metadata.contains("reasoning") &&
                        fill->metadata.is_object() && fill->metadata.contains("reasoning_options") &&
                        !model.metadata.contains("reasoning_options")) {
                        model.metadata["reasoning_options"] = fill->metadata["reasoning_options"];
                    }
                }
            } catch (...) {
                // Enrichment must never fail a listing; the unfilled model
                // degrades exactly as if no metadata catalog existed.
            }
        }
    }
    return outcome;
}

void ProvidersConfig::PublishLocked(const ProviderEntry& entry, const DiscoveryOutcome& outcome,
                                    DiscoveryCacheState::DiscoveryCache& cache) const {
    cache.attempted = true;
    cache.attempted_at = std::chrono::steady_clock::now();
    cache.generation = cache_.generation_;
    if (!outcome.catalog_present) {
        cache.ok = false;
        cache.error = "no model catalog registered for backend '" + entry.backend + "'";
        return;
    }
    if (!outcome.ok) {
        cache.ok = false;
        cache.error = outcome.error.empty() ? "catalog request failed" : outcome.error;
        return;
    }
    cache.ok = true;
    cache.error.clear();
    for (auto& model : outcome.models) {
        if (model.id.empty()) continue;  // unaddressable; not a config the caller wrote
        cache.models.insert_or_assign(model.id, std::move(model));
    }
    // The variant recipe per discovered model, computed ONCE here: the entry's
    // declared recipe (list of effort names or named bodies), or -- when the
    // entry declares none -- the model's own advertised ladder, verbatim. This
    // is the autodetection: glm-5.3-flash's catalog says supported_efforts
    // ["max","high","low"], so openrouter/z-ai/glm-5.3-flash gains @max, @high
    // and @low without a line of config. A model advertising no ladder (a bare
    // reasoning toggle, or nothing reasoning-shaped) gets none.
    // List-form entries carry no parameters of their own: MakeVariantResolved
    // derives them at sibling-build time, where the model's resolved
    // max_tokens is available for the anthropic budget clamp. Map-form
    // entries were validated at parse and pass through verbatim.
    for (const auto& [id, discovered] : cache.models) {
        std::vector<ModelVariant> declared = entry.variants;
        if (declared.empty()) {
            const auto efforts = llm::AdvertisedEfforts(discovered.metadata);
            if (efforts.empty()) continue;  // no ladder advertised: no variants
            // The model's OWN ladder, in the catalog's order -- the rungs the
            // backend says exist, autodetected. AdvertisedEfforts returns
            // non-empty only with at least one name, so the loop always fills.
            // Validated against the known vocabulary, unlike the config-file
            // path (ParseVariants) which already rejects unknown names at
            // parse time: an autodetected name outside it would otherwise
            // resolve as an addressable variant whose parameters are never
            // derived (MakeVariantResolved's IsKnownEffortName guard skips
            // it), sending a request byte-for-byte identical to the base
            // model with no warning that the selector did nothing.
            for (const auto& effort : efforts) {
                if (!IsKnownEffortName(effort)) {
                    common::DefaultLogger().warn(
                        "providers.json: model '{}' advertises an unknown effort '{}' via "
                        "discovery; skipping that variant (known: high, low, max, medium, "
                        "minimal, none, xhigh)",
                        id, effort);
                    continue;
                }
                declared.push_back(ModelVariant{effort, {}, std::nullopt, std::nullopt});
            }
        }
        // List-form entries carry no parameters of their own: MakeVariantResolved
        // derives them at sibling-build time, where the model's resolved
        // max_tokens is available for the anthropic budget clamp. Map-form
        // entries were validated at parse and pass through untouched.
        cache.variants_by_model.emplace(id, std::move(declared));
    }
    // Materialize every discovered model now: this is what puts them in
    // ModelRefs() and the /model picker's ListModels, and it means a later
    // Resolve() of any of
    // them is a lookup, never a rebuild. Values are written here -- once,
    // under the mutex, before any snapshot escapes -- and never mutated after.
    for (const auto& [id, discovered] : cache.models) {
        const ModelRef ref{entry.name, id};
        const std::string key = ref.str();
        if (catalog_.models_.count(key) || cache_.overlay_.count(key)) continue;
        cache_.overlay_.emplace(
            key, std::make_shared<const ResolvedModel>(BuildDiscoveredLocked(entry, discovered)));
        // Variant siblings materialize lazily (ResolveVariant serves them from
        // overlay_ on first resolve) -- NOT here: a discover-entry lists
        // hundreds of models, and pre-building three siblings each would trade
        // the lazy-fetch design for an eager one. The ref-keyed lookup in
        // ResolveVariant makes the cost pay-per-use.
    }
}

std::vector<std::string> ProvidersConfig::ModelRefs() const {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    // Static ∪ cached-discovered ∪ cached-discovered variant siblings. All
    // three sources are individually sorted, but a concatenation interleaves
    // wrongly ("local/zdisc" sorts before "mixed/static1" yet lives in the
    // other map) -- so merge, skipping refs a static entry already provides
    // (an enriched static model and a parsed static variant sibling both live
    // in the catalog), and sort once at the end. A cold cache contributes
    // nothing: this is a listing, never a discovery trigger.
    std::vector<std::string> out;
    out.reserve(catalog_.models_.size() + cache_.overlay_.size());
    for (const auto& [key, unused] : catalog_.models_) out.push_back(key);
    for (const auto& [key, unused] : cache_.overlay_) {
        if (!catalog_.models_.count(key)) out.push_back(key);
    }
    // The variant siblings each materialized discovery-cache model carries.
    // VariantsOfLocked applies the same precedence the resolver does: a
    // static base's own declared siblings are catalog entries already (the
    // dedup below drops the overlap), and an explicit `variants: []` opt-out
    // suppresses the autodetected ladder. The discovered-model guard skips
    // enriched static twins (their siblings come from the catalog) and a
    // variant sibling already materialized into the overlay re-derives its
    // base, which keeps a later /think switch from changing the listing.
    for (const auto& [key, unused] : cache_.overlay_) {
        (void)unused;
        const auto slash = key.find('/');
        if (slash == std::string::npos) continue;
        const std::string provider = key.substr(0, slash);
        const std::string model_id = SplitVariantSuffix(key.substr(slash + 1)).first;
        const auto cache_it = cache_.discovery_.find(provider);
        if (cache_it == cache_.discovery_.end() || !cache_it->second.ok) continue;
        if (!cache_it->second.models.count(model_id)) continue;  // an enriched static twin
        for (const auto& name : VariantsOfLocked(provider, model_id)) {
            out.push_back(provider + "/" + model_id + "@" + name);
        }
    }

    // The picker filters (opencode compat): whitelist keeps only the listed
    // model ids for that provider, then blacklist removes the listed ones.
    // Resolution is untouched -- this is a listing concern, not an access rule.
    // Providers_ is keyed by name and stable after parse, so the lookups are
    // safe without a second lock.
    bool filtered = false;
    for (const auto& [name, entry] : catalog_.providers_) {
        if (!entry.whitelist.empty() || !entry.blacklist.empty()) filtered = true;
    }
    if (filtered) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const std::string& key) {
                                     const auto slash = key.find('/');
                                     if (slash == std::string::npos) return false;
                                     const auto entry = catalog_.providers_.find(key.substr(0, slash));
                                     if (entry == catalog_.providers_.end()) return false;
                                     // A variant ref filters by its BASE id: hiding
                                     // a model must hide its variants too, and a
                                     // whitelist of base ids must keep them.
                                     const std::string model_id =
                                         SplitVariantSuffix(key.substr(slash + 1)).first;
                                     if (!entry->second.whitelist.empty() &&
                                         std::find(entry->second.whitelist.begin(),
                                                   entry->second.whitelist.end(),
                                                   model_id) == entry->second.whitelist.end()) {
                                         return true;
                                     }
                                     return std::find(entry->second.blacklist.begin(),
                                                      entry->second.blacklist.end(),
                                                      model_id) != entry->second.blacklist.end();
                                 }),
                    out.end());
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

DiscoveryStatus ProvidersConfig::discovery_status(const std::string& provider_name) const {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    const auto it = cache_.discovery_.find(provider_name);
    if (it == cache_.discovery_.end()) return {};
    return DiscoveryStatus{it->second.attempted, it->second.ok, it->second.error};
}

std::vector<DiscoveryRefreshOutcome> ProvidersConfig::RefreshDiscovery() const {
    // Create every discover-entry's gate up front, under the mutex, so the
    // fetch loop never acquires a gate and cache_.mutex_ in the wrong order.
    {
        std::lock_guard<std::mutex> lock(cache_.mutex_);
        for (const auto& [name, entry] : catalog_.providers_) {
            if (entry.discover) cache_.gates_.try_emplace(name);
        }
    }

    std::vector<DiscoveryRefreshOutcome> outcomes;
    for (const auto& [name, entry] : catalog_.providers_) {
        if (!entry.discover) continue;
        // Fetch holding only this provider's gate: refreshing a hung catalog
        // costs this provider its 10 s, never the process its config reads,
        // and distinct providers refresh in parallel.
        std::lock_guard<std::mutex> gate_lock(cache_.gates_.at(name).mtx);
        const DiscoveryOutcome outcome = FetchOutcome(entry);

        std::lock_guard<std::mutex> lock(cache_.mutex_);
        // A FAILED refresh over a previously good fetch keeps the good data:
        // the backend being down for this Ctrl+R must not empty the picker or
        // make models that resolved a minute ago "unknown". The failure is
        // reported in the outcome (and logged), not stored over the cache.
        if (!outcome.ok) {
            const auto previous = cache_.discovery_.find(name);
            if (previous != cache_.discovery_.end() && previous->second.ok) {
                const std::string error =
                    outcome.error.empty() ? "catalog request failed" : outcome.error;
                common::DefaultLogger().warn(
                    "providers.json: discovery refresh for '{}' failed ({}); keeping the "
                    "previous {} models",
                    name, error, previous->second.models.size());
                outcomes.push_back(
                    DiscoveryRefreshOutcome{name, false, error, previous->second.models.size()});
                continue;
            }
        }
        // Evict and republish atomically, under one lock acquisition: the
        // listing switches to the fresh data the moment the lock is released,
        // and readers holding snapshots of the old overlays keep those objects
        // alive -- replacing an entry never pulls it out from under a holder.
        cache_.discovery_[name] = DiscoveryCacheState::DiscoveryCache{};
        for (auto it = cache_.overlay_.begin(); it != cache_.overlay_.end();) {
            if (it->second->ref.provider == name) {
                it = cache_.overlay_.erase(it);
            } else {
                ++it;
            }
        }
        DiscoveryCacheState::DiscoveryCache& cache = cache_.discovery_.at(name);
        PublishLocked(entry, outcome, cache);
        outcomes.push_back(DiscoveryRefreshOutcome{name, cache.ok, cache.error, cache.models.size()});
    }
    return outcomes;
}

std::string DescribeModels(const ProvidersConfig& providers) {
    const auto refs = providers.ModelRefs();
    if (refs.empty()) return "no models at all (providers.json is empty)";
    std::string out;
    for (const auto& ref : refs) {
        if (!out.empty()) out += ", ";
        out += ref;
    }
    return out;
}

std::string DescribeDiscovery(const ProvidersConfig& providers, const std::string& provider_name) {
    const auto status = providers.discovery_status(provider_name);
    if (!status.attempted || status.ok) return {};
    return "; discovery for provider '" + provider_name + "' failed: " + status.error;
}

ProvidersConfig ParseProvidersConfigUnwrapped(const nlohmann::json& doc,
                                              const VariantParamsHook& variant_params_hook) {
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
    if (!doc.contains("providers")) {
        // {} is a valid empty config. Anything else with no "providers" key is
        // almost certainly a typo ("provider", "Providers", ...) -- and the
        // failure used to surface much later, as "preset names unknown model
        // ... providers.json is empty", pointing the reader at the preset
        // instead of their own key. Refuse here, where the mistake is.
        if (!doc.empty()) {
            const std::string found = doc.begin().key();
            throw std::runtime_error(
                "providers.json: expected a \"providers\" object, found no such key (first key "
                "present: \"" +
                found +
                "\"). An empty object {} is a valid empty config; a typo'd key is not.");
        }
        return config;
    }
    const auto& providers = doc.at("providers");
    if (!providers.is_object()) {
        throw std::runtime_error("providers.json: \"providers\" must be an object keyed by name");
    }

    for (const auto& [provider_name, provider] : providers.items()) {
        const std::string where = "provider '" + provider_name + "'";
        if (!provider.is_object()) {
            throw std::runtime_error("providers.json: " + where + " must be an object");
        }
        const nlohmann::json provider_normalized = NormalizeOpencodeProvider(provider, where);

        const std::string backend = RequireString(provider_normalized, "backend", where);
        const std::string url = RequireString(provider_normalized, "url", where);
        const ApiKeyResolution api_key = ResolveApiKey(provider_normalized, where, provider_name);
        // One line per entry, naming the source but never the key: this is
        // the moment the key is frozen in for the process's lifetime, so an
        // empty resolution must be visible HERE, not only as a 401 on the
        // first request.
        if (!api_key.key.empty()) {
            common::DefaultLogger().info("providers.json: {} -- api key from {}",
                                         where, api_key.origin);
        } else if (api_key.auth_file_opt_in) {
            common::DefaultLogger().warn(
                "providers.json: {} -- opted into \"credentials\": \"auth-file\" but no key was "
                "found in {} or ~/.local/share/opencode/auth.json. Credentials resolve once at "
                "startup: if the key was written after the server started (e.g. via /connect), "
                "restart the server.",
                where, (common::DataDir() / "auth.json").string());
        }

        bool discover = false;
        if (provider_normalized.contains("discover")) {
            if (!provider_normalized.at("discover").is_boolean()) {
                throw std::runtime_error("providers.json: " + where +
                                         " sets \"discover\" which must be a boolean");
            }
            discover = provider_normalized.at("discover").get<bool>();
        }

        // Metadata enrichment knobs (docs/EXTENSIBILITY.md §2.13). On by
        // default: the whole point of the seam is that a model missing its
        // window/efforts gets them without a line of config, so the only
        // spelling that matters here is the opt-out. A non-boolean is a named
        // parse error, like "discover" above.
        bool metadata = true;
        if (provider_normalized.contains("metadata")) {
            if (!provider_normalized.at("metadata").is_boolean()) {
                throw std::runtime_error("providers.json: " + where +
                                         " sets \"metadata\" which must be a boolean");
            }
            metadata = provider_normalized.at("metadata").get<bool>();
        }
        // Per-entry id-matching policy override. The config-wide default is
        // Full (the conservative choice); an entry that serves ids with dated
        // suffixes or vendor prefixes can loosen it locally. Anything but the
        // two known names is a named parse error -- the mistake must not
        // silently read as "full".
        llm::MetadataMatch metadata_match = llm::MetadataMatch::Full;
        if (provider_normalized.contains("metadata_match")) {
            const auto& match = provider_normalized.at("metadata_match");
            if (!match.is_string() ||
                (match.get<std::string>() != "full" && match.get<std::string>() != "substring")) {
                throw std::runtime_error(
                    "providers.json: " + where +
                    " has an invalid \"metadata_match\": expected \"full\" or \"substring\"");
            }
            metadata_match = match.get<std::string>() == "substring"
                                 ? llm::MetadataMatch::Substring
                                 : llm::MetadataMatch::Full;
        }
        // The catalog-side provider-table pin. Opaque here -- MetadataQuery
        // carries it verbatim and the catalog validates its own spelling.
        std::string metadata_provider;
        if (provider_normalized.contains("metadata_provider")) {
            const auto& pin = provider_normalized.at("metadata_provider");
            if (!pin.is_string() || pin.get<std::string>().empty()) {
                throw std::runtime_error("providers.json: " + where +
                                         " sets \"metadata_provider\" which must be a non-empty "
                                         "string (the catalog's provider name)");
            }
            metadata_provider = pin.get<std::string>();
        }

        const bool has_models = provider_normalized.contains("models") &&
                                provider_normalized.at("models").is_object() &&
                                !provider_normalized.at("models").empty();
        if (!has_models && !discover) {
            // A provider with no models is unaddressable -- nothing could name it
            // -- so it is a mistake rather than an empty-but-valid state. A
            // discover-entry is the exception: its catalog names the models.
            throw std::runtime_error("providers.json: " + where +
                                     " has no \"models\"; a provider serving no model cannot be "
                                     "addressed as <provider>/<model> (or set \"discover\": true "
                                     "to list them from the backend)");
        }

        // The connection and provider-level defaults, retained for discovery:
        // a discovered model inherits them exactly as a static one does.
        ProviderEntry entry;
        entry.name = provider_name;
        entry.backend = backend;
        entry.connection.base_url = url;
        entry.connection.api_key = api_key.key;
        entry.api_key_origin = api_key.origin;
        entry.discover = discover;
        entry.metadata = metadata;
        entry.metadata_match = metadata_match;
        entry.metadata_provider = metadata_provider;
        if (provider_normalized.contains("parameters") &&
            provider_normalized.at("parameters").is_object()) {
            entry.parameters = provider_normalized.at("parameters");
        }
        entry.max_tokens = OwnNumber<int>(provider_normalized, "max_tokens");
        entry.context_window_tokens =
            OwnNumber<std::int64_t>(provider_normalized, "context_window_tokens");
        // The entry-level variants recipe (list of effort names or named
        // bodies), applied to every model under the entry -- static and
        // discovered alike. For a discover-entry this IS the autodiscovery
        // recipe; absent, discovery defaults to "@high" for models whose
        // catalog metadata advertises efforts. ParseVariants itself refuses
        // any other shape with the same message.
        if (provider_normalized.contains("variants")) {
            if (!provider_normalized.at("variants").is_array() &&
                !provider_normalized.at("variants").is_object()) {
                throw std::runtime_error(
                    "providers.json: " + where +
                    " field \"variants\" must be an array of effort names or an object of "
                    "named variant bodies, got " + provider_normalized.at("variants").dump());
            }
            entry.variants = ParseVariants(provider_normalized.at("variants"), backend,
                                           where + " (provider)");
        }
        // The output-cap key spelling for the OpenAI-shaped request body.
        // Empty (absent) means the historical "max_tokens"; the only other
        // value accepted is "max_completion_tokens" (newer OpenAI models
        // reject the legacy key). A typo'd value is a named parse error, not
        // a silent pass-through that no backend understands.
        if (provider_normalized.contains("max_tokens_field")) {
            const auto& field = provider_normalized.at("max_tokens_field");
            if (!field.is_string() ||
                (field.get<std::string>() != "max_tokens" &&
                 field.get<std::string>() != "max_completion_tokens")) {
                throw std::runtime_error(
                    "providers.json: " + where +
                    " has an invalid \"max_tokens_field\": expected \"max_tokens\" or "
                    "\"max_completion_tokens\"");
            }
            entry.connection.max_tokens_field = field.get<std::string>();
        }
        // Transport timeouts, all optional, all in milliseconds. A local
        // backend prefilling a huge prompt needs a longer first_byte than the
        // defaults; a wrong type or a non-positive value is a named parse
        // error rather than a silently ignored key.
        if (provider_normalized.contains("timeouts")) {
            const auto& timeouts = provider_normalized.at("timeouts");
            if (!timeouts.is_object()) {
                throw std::runtime_error("providers.json: " + where +
                                         " field \"timeouts\" must be an object");
            }
            const auto read_ms = [&](const char* key, std::chrono::milliseconds& target) {
                if (!timeouts.contains(key)) return;
                const auto& value = timeouts.at(key);
                constexpr std::int64_t kMaxMs = 24LL * 3600 * 1000;
                if (!value.is_number_integer() || value.get<std::int64_t>() <= 0 ||
                    value.get<std::int64_t>() > kMaxMs) {
                    throw std::runtime_error("providers.json: " + where + " timeouts." + key +
                                             " must be an integer in 1.." +
                                             std::to_string(kMaxMs) + " (milliseconds)");
                }
                target = std::chrono::milliseconds(value.get<std::int64_t>());
            };
            read_ms("connect_ms", entry.connection.connect_timeout);
            read_ms("request_ms", entry.connection.request_timeout);
            read_ms("stream_idle_ms", entry.connection.stream_idle_timeout);
            read_ms("first_byte_ms", entry.connection.first_byte_timeout);
        }
        if (provider_normalized.contains("headers") && provider_normalized.at("headers").is_array()) {
            for (const auto& pair : provider_normalized.at("headers")) {
                if (pair.is_array() && pair.size() == 2 && pair[0].is_string() &&
                    pair[1].is_string()) {
                    entry.connection.headers.emplace_back(pair[0].get<std::string>(),
                                                          pair[1].get<std::string>());
                }
            }
        }
        ApplyAppAttribution(provider_normalized, entry.connection, where);
        for (const char* key : {"whitelist", "blacklist"}) {
            if (provider_normalized.contains(key) && provider_normalized.at(key).is_array()) {
                for (const auto& id : provider_normalized.at(key)) {
                    if (id.is_string()) (key[0] == 'w' ? entry.whitelist : entry.blacklist)
                                           .push_back(id.get<std::string>());
                }
            }
        }
        // Checked for every entry, discover-only ones included: discovered
        // models are addressed as <provider>/<model> too, so a '/' in the
        // name makes the reference ambiguous whether or not models are listed.
        if (provider_name.find('/') != std::string::npos) {
            throw std::runtime_error("providers.json: provider '" + provider_name +
                                     "' contains a '/', which makes the reference ambiguous");
        }
        config.AddProvider(entry);

        if (!has_models) continue;  // discover-only entry: nothing static to resolve

        for (const auto& [model_name, model] : provider_normalized.at("models").items()) {
            const std::string model_where = "model '" + provider_name + "/" + model_name + "'";
            if (!model.is_object()) {
                throw std::runtime_error("providers.json: " + model_where + " must be an object");
            }
            // A model KEY may contain '/', split at the FIRST one -- the same
            // rule SplitModelRef applies to interactive refs and the
            // ModelRefs() picker filter applies to catalog keys. This is what
            // makes static entries express OpenRouter ids ("anthropic/
            // claude-opus-5" under provider "openrouter" resolves as
            // openrouter/anthropic/claude-opus-5 and is sent on the wire
            // verbatim, exactly as discovery's slash-bearing ids are). Empty
            // halves are still refused: a name with a dangling slash is a
            // typo, and "a/" as a wire model id would 400 with no hint why.
            if (const auto slash = model_name.find('/'); slash == 0 || slash + 1 == model_name.size()) {
                throw std::runtime_error(
                    "providers.json: model '" + provider_name + "/" + model_name +
                    "' has an empty half -- a model key may contain '/' (OpenRouter ids like "
                    "\"anthropic/claude-opus-5\"), but it must not be empty, and both sides of its "
                    "first '/' must be non-empty");
            }

            ResolvedModel resolved;
            resolved.ref = ModelRef{provider_name, model_name};
            resolved.backend = backend;
            resolved.provider.base_url = url;
            resolved.provider.api_key = api_key.key;
            resolved.provider.model_name = model_name;
            resolved.provider.headers = entry.connection.headers;
            resolved.provider.max_tokens_field = entry.connection.max_tokens_field;
            resolved.provider.connect_timeout = entry.connection.connect_timeout;
            resolved.provider.request_timeout = entry.connection.request_timeout;
            resolved.provider.stream_idle_timeout = entry.connection.stream_idle_timeout;
            resolved.provider.first_byte_timeout = entry.connection.first_byte_timeout;
            resolved.parameters = InheritParameters(provider, model);
            resolved.max_tokens = InheritNumber<int>(provider, model, "max_tokens", model_where);
            resolved.context_window_tokens =
                InheritNumber<std::int64_t>(provider, model, "context_window_tokens",
                                            model_where);
            resolved.variants_declared = HasDeclaredVariants(provider, model);
            // The key is copied out FIRST. Passing `resolved.ref` alongside
            // `std::move(resolved)` in one call is unsequenced -- the move may
            // run first, and the key is then read from a moved-from string.
            const ModelRef key = resolved.ref;
            // Effort variants: the model's own `variants` (either spelling),
            // else the provider's. Each declared variant becomes a SIBLING
            // ResolvedModel -- same connection and wire id, parameters
            // overlaid -- addressable as <provider>/<model>@<name>. Declared
            // names are validated (list form: known efforts; map form:
            // well-formed bodies) by ParseVariants above, so a sibling that
            // exists here is always resolvable. Synthesized BEFORE the move
            // below: the sibling is a copy of `resolved`, and a moved-from
            // base would publish garbage parameters.
            for (const auto& variant : InheritVariants(provider, model, backend, model_where)) {
                ResolvedModel sibling = MakeVariantResolved(resolved, variant, variant_params_hook);
                const ModelRef sibling_key = sibling.ref;
                config.Add(sibling_key, std::move(sibling));
            }
            config.Add(key, std::move(resolved));
        }
    }
    return config;
}

ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc,
                                     const VariantParamsHook& variant_params_hook) {
    // ParseProvidersConfigUnwrapped already throws well-formed
    // "providers.json: ..." runtime_errors for every schema violation it
    // anticipates (checked with contains()/is_*() before every get<T>()); this
    // only catches a stray nlohmann::json::exception a future unguarded get<T>()
    // might leak, normalizing it the same way settings/permissions self-wrap.
    try {
        return ParseProvidersConfigUnwrapped(doc, variant_params_hook);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("providers.json: ") + error.what());
    }
}

ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path,
                                    const VariantParamsHook& variant_params_hook) {
    // LoadJsonConfig's parse callback is single-argument (const json&); the
    // hook rides a capture instead of a second callback parameter.
    return LoadJsonConfig<ProvidersConfig>(
        path, "providers", AbsencePolicy::kThrow, EmptyFilePolicy::kTreatAsMalformed,
        [&variant_params_hook](const nlohmann::json& doc) {
            return ParseProvidersConfig(doc, variant_params_hook);
        });
}

}  // namespace minimal::config
