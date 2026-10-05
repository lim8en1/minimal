#include "minimal/config/providers_config.hpp"
#include "minimal/config/providers_internal.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/common/secret_handle.hpp"
#include "minimal/config/json_config_loader.hpp"
#include "minimal/config/parse_util.hpp"
#include "minimal/config/provider_config_extensions.hpp"
#include "minimal/config/secrets.hpp"

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

// Comment keys ("//", "//2", ...) are the file's annotation convention.
bool IsCommentKey(const std::string& key) { return key.rfind("//", 0) == 0; }

// Strict schema (config::RejectUnknownKeys), with the two fields the secrets
// rework removed named outright: they are what an older file still carries.
void RejectUnknownFields(const nlohmann::json& node, std::initializer_list<std::string_view> known,
                         const std::string& where) {
    for (const char* removed : {"api_key_env", "credentials"}) {
        if (node.contains(removed)) {
            throw std::runtime_error("providers.json: " + where + " field \"" + removed +
                                     "\" was removed; name the key with \"api_key\": "
                                     "\"env:NAME\" or \"secret:NAME\" (docs/CONFIGURATION.md, "
                                     "Secrets)");
        }
    }
    RejectUnknownKeys(node, known, "providers.json: " + where);
}

// The entry's `api_key`, bound to a handle that reads it per request (a
// secret stored after startup reaches the next request). A reference that can
// never resolve -- malformed, an unset "env:" variable -- fails here, at load,
// rather than as a 401 hours later.
common::SecretHandle BindApiKey(const nlohmann::json& provider, const std::string& where,
                                const std::string& url, const SecretResolver& secrets) {
    if (!provider.contains("api_key")) return {};
    if (!provider.at("api_key").is_string()) {
        throw std::runtime_error("providers.json: " + where + " field \"api_key\" must be a string");
    }
    try {
        return secrets.Bind(provider.at("api_key").get<std::string>(), url);
    } catch (const std::exception& error) {
        throw std::runtime_error("providers.json: " + where + " api_key: " + error.what());
    }
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


// Connection headers: providers.json accepts an object or a list of
// [name, value] pairs; both normalize to the pair list. Scalars are coerced
// (a silent drop would turn "unusual but working" into an auth failure with
// no trace), non-scalars dropped with a warning.
nlohmann::json NormalizeHeaders(const nlohmann::json& provider) {
    if (!provider.is_object() || !provider.contains("headers") ||
        !provider.at("headers").is_object()) {
        return provider;
    }
    nlohmann::json out = provider;
    nlohmann::json flat = nlohmann::json::array();
    for (const auto& [name, value] : out.at("headers").items()) {
        if (value.is_string()) {
            flat.push_back(nlohmann::json::array({name, value}));
        } else if (value.is_number_integer() || value.is_number_float() || value.is_boolean()) {
            flat.push_back(nlohmann::json::array(
                {name, value.is_boolean() ? std::string(value.get<bool>() ? "true" : "false")
                                          : value.dump()}));
        } else {
            common::DefaultLogger().warn(
                "providers.json: header '{}' has a non-scalar value and was dropped", name);
        }
    }
    out["headers"] = std::move(flat);
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
                                  const VariantParamsHook& variant_params_hook) {
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

// The overlay ResolvedModel for one discovered model: the entry's connection
// and provider-level defaults, the backend's id, window, and output cap.
// A pure function of its two parameters.
ResolvedModel BuildDiscoveredModel(const ProviderEntry& entry,
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

ProvidersConfig& ProvidersConfig::operator=(ProvidersConfig&& other) noexcept {
    if (this == &other) return *this;  // MoveFrom would lock the same mutex twice
    catalog_.models_ = std::move(other.catalog_.models_);
    catalog_.providers_ = std::move(other.catalog_.providers_);
    service_.MoveRuntimeFrom(std::move(other.service_));
    return *this;
}

void ProvidersConfig::Add(const ModelRef& ref, ResolvedModel resolved) {
    catalog_.models_.insert_or_assign(ref.str(),
                                      std::make_shared<const ResolvedModel>(std::move(resolved)));
}

void ProvidersConfig::AddProvider(ProviderEntry entry) {
    catalog_.providers_.insert_or_assign(entry.name, std::move(entry));
}

// --- runtime API: thin delegators to the ModelCatalogService --------------------

std::vector<std::string> ProvidersConfig::VariantsOf(const ModelRef& ref) const {
    return service_.VariantsOf(ref);
}
void ProvidersConfig::SetDiscoveryHook(DiscoveryHook hook) {
    service_.SetDiscoveryHook(std::move(hook));
}
void ProvidersConfig::SetDiscoveryRetryBackoff(std::chrono::milliseconds backoff) {
    service_.SetDiscoveryRetryBackoff(backoff);
}
void ProvidersConfig::SetMetadataHook(MetadataHook hook) {
    service_.SetMetadataHook(std::move(hook));
}
void ProvidersConfig::SetVariantParamsHook(VariantParamsHook hook) {
    service_.SetVariantParamsHook(std::move(hook));
}
std::shared_ptr<const ResolvedModel> ProvidersConfig::Resolve(const ModelRef& ref) const {
    return service_.Resolve(ref);
}
std::vector<std::string> ProvidersConfig::ModelRefs() const { return service_.ModelRefs(); }
DiscoveryStatus ProvidersConfig::discovery_status(const std::string& provider_name) const {
    return service_.discovery_status(provider_name);
}
std::vector<DiscoveryRefreshOutcome> ProvidersConfig::RefreshDiscovery() const {
    return service_.RefreshDiscovery();
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
                                              const ProviderConfigExtensions* extensions,
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
    RejectUnknownFields(doc, {"providers"}, "the top level");
    const auto& providers = doc.at("providers");
    if (!providers.is_object()) {
        throw std::runtime_error("providers.json: \"providers\" must be an object keyed by name");
    }
    const SecretResolver secrets(SecretStorePath(), extensions);

    for (const auto& [provider_name, provider] : providers.items()) {
        const std::string where = "provider '" + provider_name + "'";
        if (!provider.is_object()) {
            throw std::runtime_error("providers.json: " + where + " must be an object");
        }
        nlohmann::json translated = provider;
        if (extensions != nullptr) extensions->Translate(translated, where);
        const nlohmann::json provider_normalized = NormalizeHeaders(translated);
        // After translation: a plugin translator consumes its own tool's
        // fields, so whatever is left must be core's.
        RejectUnknownFields(provider_normalized,
                            {"backend", "url", "api_key", "discover", "metadata",
                             "metadata_match", "metadata_provider", "models", "parameters",
                             "max_tokens", "context_window_tokens", "variants",
                             "max_tokens_field", "timeouts", "headers", "openrouter_app_id",
                             "whitelist", "blacklist"},
                            where);

        const std::string backend = RequireString(provider_normalized, "backend", where);
        const std::string url = RequireString(provider_normalized, "url", where);
        const common::SecretHandle api_key = BindApiKey(provider_normalized, where, url, secrets);
        // One line per entry, naming the source but never the key. A stored
        // secret not set yet is a warning, not an error: /secret set fixes it
        // for the next request, no restart.
        if (api_key.configured()) {
            try {
                common::DefaultLogger().info("providers.json: {} -- api key from {}", where,
                                             api_key.Get().source);
            } catch (const std::exception& error) {
                common::DefaultLogger().warn(
                    "providers.json: {} -- api key {} does not resolve yet ({}); requests fail "
                    "until it does",
                    where, api_key.description(), error.what());
            }
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
        entry.connection.api_key = api_key;
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
            RejectUnknownFields(timeouts,
                                {"connect_ms", "request_ms", "stream_idle_ms", "first_byte_ms"},
                                where + " timeouts");
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
            if (IsCommentKey(model_name)) continue;
            const std::string model_where = "model '" + provider_name + "/" + model_name + "'";
            if (!model.is_object()) {
                throw std::runtime_error("providers.json: " + model_where + " must be an object");
            }
            RejectUnknownFields(model,
                                {"parameters", "max_tokens", "context_window_tokens", "variants"},
                                model_where);
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
            resolved.provider.api_key = api_key;
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
                                     const VariantParamsHook& variant_params_hook,
                                     const ProviderConfigExtensions* extensions) {
    // ParseProvidersConfigUnwrapped already throws well-formed
    // "providers.json: ..." runtime_errors for every schema violation it
    // anticipates (checked with contains()/is_*() before every get<T>()); this
    // only catches a stray nlohmann::json::exception a future unguarded get<T>()
    // might leak, normalizing it the same way settings/permissions self-wrap.
    try {
        return ParseProvidersConfigUnwrapped(doc, extensions, variant_params_hook);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("providers.json: ") + error.what());
    }
}

ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path,
                                    const VariantParamsHook& variant_params_hook,
                                    const ProviderConfigExtensions* extensions) {
    // LoadJsonConfig's parse callback is single-argument (const json&); the
    // hook rides a capture instead of a second callback parameter.
    return LoadJsonConfig<ProvidersConfig>(
        path, "providers", AbsencePolicy::kThrow, EmptyFilePolicy::kTreatAsMalformed,
        [&variant_params_hook, extensions](const nlohmann::json& doc) {
            return ParseProvidersConfig(doc, variant_params_hook, extensions);
        });
}

}  // namespace minimal::config
