#include "minimal/config/providers_config.hpp"
#include "minimal/config/providers_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/common/secret_handle.hpp"
#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/config/provider_config_extensions.hpp"
#include "minimal/config/secrets.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::config {
namespace {

// The model's value if it has one, else the provider's. Shallow by design: a
// model overriding `parameters` replaces the object rather than merging keys into
// it, so what a model declares is exactly what it sends. Deep-merging would make
// a model's stated settings depend on a provider block elsewhere in the file.
nlohmann::json InheritParameters(const std::optional<nlohmann::json>& provider,
                                 const std::optional<nlohmann::json>& model) {
    if (model) return *model;
    if (provider) return *provider;
    return nlohmann::json::object();
}

std::optional<nlohmann::json> MaybeParameters(Reader& reader) {
    if (auto parameters = reader.MaybeObject("parameters")) {
        return std::optional<nlohmann::json>(std::in_place, parameters->node());
    }
    return std::nullopt;
}

// --- variants parsing --------------------------------------------------------

// The `variants` key on a model or provider, either spelling: an ARRAY of
// effort names (parameters derived per backend at synthesis time) or an OBJECT
// of named full bodies. nullopt when the key is absent.
std::optional<std::vector<ModelVariant>> MaybeVariants(Reader& owner) {
    const auto* node = owner.Raw("variants");
    if (node == nullptr) return std::nullopt;
    std::vector<ModelVariant> out;
    if (node->is_array()) {
        for (const auto& item : *node) {
            if (!item.is_string()) {
                owner.Fail("variants", "must contain only effort names, got " + item.dump());
            }
            const std::string name = item.get<std::string>();
            if (!IsKnownEffortName(name)) {
                owner.Fail("variants", "names '" + name +
                                           "', which is not a known effort (known: low, medium, "
                                           "high, xhigh, max, minimal, none)");
            }
            // No parameters here: MakeVariantResolved derives the wire map from
            // the name at sibling-build time, where the base model's resolved
            // max_tokens is available for the anthropic budget clamp.
            out.push_back(ModelVariant{name, nlohmann::json::object(), std::nullopt,
                                       std::nullopt});
        }
        return out;
    }
    if (!node->is_object()) {
        owner.Fail("variants", "must be a list of effort names or an object of named variant "
                               "bodies, got " + node->dump());
    }
    Reader variants = owner.Child(*node, "variants");
    for (const auto& [name, body] : node->items()) {
        if (IsCommentKey(name)) continue;
        variants.Accept(name);
        if (name.empty() || name.find('@') != std::string::npos) {
            variants.Fail(name, "a variant name must be non-empty and must not contain '@'");
        }
        if (!body.is_object()) variants.Fail(name, "must be an object");
        Reader reader = variants.Child(body, name);
        ModelVariant variant;
        variant.name = name;
        if (auto parameters = MaybeParameters(reader)) variant.parameters = *parameters;
        if (const auto cap = reader.MaybeInt("max_tokens", 1, std::numeric_limits<int>::max())) {
            variant.max_tokens = static_cast<int>(*cap);
        }
        variant.context_window_tokens = reader.MaybeInt("context_window_tokens", 1);
        reader.Finish();
        out.push_back(std::move(variant));
    }
    return out;
}

// Connection headers: an object (scalar values coerced to text) or a list of
// [name, value] string pairs. Anything else is an error naming the header.
std::vector<std::pair<std::string, std::string>> ParseHeaders(Reader& provider) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto* node = provider.Raw("headers");
    if (node == nullptr) return out;
    const auto check = [&](const std::string& name, const std::string& value) {
        const auto crlf = [](const std::string& text) {
            return text.find_first_of("\r\n") != std::string::npos;
        };
        if (name.empty() || crlf(name) || crlf(value)) {
            provider.Fail("headers", "header '" + name +
                                         "' must have a non-empty name and no CR/LF");
        }
        out.emplace_back(name, value);
    };
    if (node->is_object()) {
        for (const auto& [name, value] : node->items()) {
            if (IsCommentKey(name)) continue;
            if (value.is_string()) {
                check(name, value.get<std::string>());
            } else if (value.is_number() || value.is_boolean()) {
                check(name, value.dump());
            } else {
                provider.Fail("headers", "header '" + name + "' must have a scalar value, got " +
                                             value.dump());
            }
        }
        return out;
    }
    if (node->is_array()) {
        for (const auto& pair : *node) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() ||
                !pair[1].is_string()) {
                provider.Fail("headers", "list entries must be [name, value] string pairs, got " +
                                             pair.dump());
            }
            check(pair[0].get<std::string>(), pair[1].get<std::string>());
        }
        return out;
    }
    provider.Fail("headers", "must be an object or a list of [name, value] pairs, got " +
                                 node->dump());
}

// The entry's `api_key`, bound to a handle that reads it per request (a
// secret stored after startup reaches the next request). A malformed
// reference fails here, at load, rather than as a 401 hours later.
common::SecretHandle BindApiKey(Reader& provider, const std::string& url,
                                const SecretResolver& secrets) {
    const auto ref = provider.MaybeString("api_key", /*allow_empty=*/false);
    if (!ref) return {};
    try {
        return secrets.Bind(*ref, url);
    } catch (const std::exception& error) {
        provider.Fail("api_key", error.what());
    }
}

}  // namespace

// --- effort variants ---------------------------------------------------------

VariantParamsHook MakeVariantParamsHook(const llm::VariantParamsRegistry* registry) {
    if (registry == nullptr) return {};
    return [registry](const std::string& kind, const std::string& effort_word,
                      std::optional<int> max_tokens, const nlohmann::json& metadata)
               -> std::optional<std::pair<nlohmann::json, bool>> {
        auto* provider = registry->Find(kind);
        if (provider == nullptr) return std::nullopt;
        return provider->DeriveParameters(effort_word, max_tokens, metadata);
    };
}

EffortsHook MakeEffortsHook(const llm::VariantParamsRegistry* registry) {
    if (registry == nullptr) return {};
    return [registry](const std::string& kind,
                      const nlohmann::json& metadata) -> std::optional<std::vector<std::string>> {
        auto* provider = registry->Find(kind);
        if (provider == nullptr) return std::nullopt;
        return provider->AdvertisedEfforts(metadata);
    };
}

std::vector<std::string> EffortsFor(const std::string& backend_kind, const nlohmann::json& metadata,
                                    const EffortsHook& hook) {
    if (hook) {
        if (auto own = hook(backend_kind, metadata)) return *own;
    }
    return llm::AdvertisedEfforts(metadata);
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
                                                     const VariantParamsHook& hook,
                                                     const nlohmann::json& metadata) {
    // A plugin-registered mapping sees the effort name verbatim, so a backend
    // with a longer ladder (anthropic's thinking budgets) keeps every rung.
    if (hook) {
        if (auto derived = hook(backend, effort, max_tokens, metadata)) return *derived;
        // No mapping registered for `backend`: the generic shape below.
    }
    // The generic reasoning_effort ladder tops out at "high", and has no "off"
    // spelling every backend accepts -- send the toggle word instead (a
    // toggle-only model always thinks).
    std::string word = effort;
    if (word == "xhigh" || word == "max") word = "high";
    if (word == "none") word = "minimal";
    nlohmann::json out;
    out["reasoning_effort"] = word;
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
// the base came from the static parse or discovery. MaybeVariants only
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
                                  const VariantParamsHook& variant_params_hook,
                                  const nlohmann::json& metadata) {
    ResolvedModel sibling = base;
    sibling.ref = ModelRef{base.ref.provider, base.ref.model + "@" + variant.name};
    // THE WIRE ID STAYS THE BASE'S. agent_loop sends provider.model_name
    // verbatim; the @suffix is addressing only and must never reach the wire.
    sibling.provider.model_name = base.provider.model_name;
    nlohmann::json parameters = variant.parameters;
    if (parameters.empty() && IsKnownEffortName(variant.name)) {
        const auto [derived, clamped] = VariantParametersFor(
            base.backend, variant.name, base.max_tokens, variant_params_hook, metadata);
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
    // a static model gives its own value over the provider's.
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

void ProvidersConfig::SetEffortsHook(EffortsHook hook) { service_.SetEffortsHook(std::move(hook)); }
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
    ProvidersConfig config;
    Reader root(doc, "providers.json");
    const auto provider_entries = root.Entries("providers");
    root.Finish();
    if (provider_entries.empty()) return config;  // {} is a valid empty config
    const SecretResolver secrets(SecretStorePath(), extensions);

    for (const auto& [provider_name, provider_node] : provider_entries) {
        const std::string segment = "providers." + provider_name;
        if (provider_name.empty() || provider_name.find('/') != std::string::npos ||
            provider_name.find('@') != std::string::npos) {
            root.Fail(segment, "a provider name must be non-empty and contain no '/' or '@' "
                               "(models are addressed as <provider>/<model>)");
        }
        if (!provider_node->is_object()) root.Fail(segment, "must be an object");
        // A translator rewrites its own tool's fields first; whatever is left
        // must be core's, and core validates all of it.
        nlohmann::json translated = *provider_node;
        if (extensions != nullptr) extensions->Translate(translated, "provider '" + provider_name + "'");
        Reader provider = root.Child(translated, segment);

        const std::string backend = provider.RequireString("backend");
        const std::string url = provider.RequireString("url");
        const common::SecretHandle api_key = BindApiKey(provider, url, secrets);
        // One line per entry, naming the source but never the key. A key that
        // does not resolve yet (a secret not stored, an unset env variable) is
        // a warning, not an error: requests through this entry fail naming the
        // fix, and every other provider keeps working.
        if (api_key.configured()) {
            try {
                common::DefaultLogger().info("providers.json: {} -- api key from {}", segment,
                                             api_key.Get().source);
            } catch (const std::exception& error) {
                common::DefaultLogger().warn(
                    "providers.json: {} -- api key {} does not resolve ({}); requests fail "
                    "until it does",
                    segment, api_key.description(), error.what());
            }
        }

        // The connection and provider-level defaults, retained for discovery:
        // a discovered model inherits them exactly as a static one does.
        ProviderEntry entry;
        entry.name = provider_name;
        entry.backend = backend;
        entry.connection.base_url = url;
        entry.connection.api_key = api_key;
        entry.discover = provider.Bool("discover", false);
        // Metadata enrichment (docs/EXTENSIBILITY.md §2.13): on by default, so
        // the only spelling that matters is the opt-out.
        entry.metadata = provider.Bool("metadata", true);
        entry.metadata_match = provider.Enum("metadata_match", "full", {"full", "substring"}) ==
                                       "substring"
                                   ? llm::MetadataMatch::Substring
                                   : llm::MetadataMatch::Full;
        // The catalog-side provider-table pin; the catalog validates its spelling.
        entry.metadata_provider =
            provider.MaybeString("metadata_provider", /*allow_empty=*/false).value_or("");
        const auto provider_parameters = MaybeParameters(provider);
        if (provider_parameters) entry.parameters = *provider_parameters;
        if (const auto cap =
                provider.MaybeInt("max_tokens", 1, std::numeric_limits<int>::max())) {
            entry.max_tokens = static_cast<int>(*cap);
        }
        entry.context_window_tokens = provider.MaybeInt("context_window_tokens", 1);
        // The entry-level variants recipe, applied to every model under the
        // entry -- static and discovered alike.
        const auto provider_variants = MaybeVariants(provider);
        if (provider_variants) entry.variants = *provider_variants;
        // The output-cap key spelling for the OpenAI-shaped request body;
        // newer OpenAI models reject the legacy key.
        if (const auto field =
                provider.MaybeEnum("max_tokens_field", {"max_tokens", "max_completion_tokens"})) {
            entry.connection.max_tokens_field = *field;
        }
        // Off by default: it costs a second GET per discovery fetch, and only
        // LiteLLM-shaped gateways serve the endpoint.
        entry.connection.fetch_model_info = provider.Bool("model_info", false);
        // Transport timeouts, all optional, all in milliseconds.
        if (auto timeouts = provider.MaybeObject("timeouts")) {
            const auto read_ms = [&](const char* key, std::chrono::milliseconds& target) {
                if (const auto ms = timeouts->MaybeInt(key, 1, common::kMaxDeadline.count())) {
                    target = std::chrono::milliseconds(*ms);
                }
            };
            read_ms("connect_ms", entry.connection.connect_timeout);
            read_ms("request_ms", entry.connection.request_timeout);
            read_ms("stream_idle_ms", entry.connection.stream_idle_timeout);
            read_ms("first_byte_ms", entry.connection.first_byte_timeout);
            timeouts->Finish();
        }
        entry.connection.headers = ParseHeaders(provider);
        // OpenRouter app attribution: headers synthesized here; an explicit
        // "headers" entry of the same name wins.
        if (provider.Bool("openrouter_app_id", false)) {
            const std::pair<const char*, const char*> synthesized[] = {
                {"X-Title", kAppAttributionTitle},
                {"HTTP-Referer", kAppAttributionReferer},
            };
            for (const auto& [name, value] : synthesized) {
                const bool present = std::any_of(
                    entry.connection.headers.begin(), entry.connection.headers.end(),
                    [&](const auto& header) { return header.first == name; });
                if (!present) entry.connection.headers.emplace_back(name, value);
            }
        }
        entry.whitelist = provider.StringList("whitelist");
        entry.blacklist = provider.StringList("blacklist");

        const auto models = provider.Entries("models");
        if (models.empty() && !entry.discover) {
            // A provider with no models is unaddressable; a discover-entry is
            // the exception: its catalog names the models.
            provider.Fail("models", "is missing or empty; a provider serving no model cannot be "
                                    "addressed as <provider>/<model> (or set \"discover\": true "
                                    "to list them from the backend)");
        }
        provider.Finish();
        config.AddProvider(entry);

        for (const auto& [model_name, model_node] : models) {
            const std::string model_segment = "models." + model_name;
            // A model KEY may contain '/', split at the FIRST one: it is sent on
            // the wire verbatim, so OpenRouter ids ("anthropic/claude-opus-5")
            // are expressible. Empty halves are a typo.
            if (const auto slash = model_name.find('/');
                model_name.empty() || slash == 0 || slash + 1 == model_name.size()) {
                provider.Fail(model_segment,
                              "a model key must be non-empty, and both sides of its first '/' "
                              "must be non-empty");
            }
            if (!model_node->is_object()) provider.Fail(model_segment, "must be an object");
            Reader model = provider.Child(*model_node, model_segment);

            ResolvedModel resolved;
            resolved.ref = ModelRef{provider_name, model_name};
            resolved.backend = backend;
            resolved.provider = entry.connection;
            resolved.provider.model_name = model_name;
            resolved.parameters = InheritParameters(provider_parameters, MaybeParameters(model));
            const auto model_cap = model.MaybeInt("max_tokens", 1, std::numeric_limits<int>::max());
            resolved.max_tokens = model_cap ? std::optional<int>(static_cast<int>(*model_cap))
                                            : entry.max_tokens;
            const auto model_window = model.MaybeInt("context_window_tokens", 1);
            resolved.context_window_tokens = model_window ? model_window
                                                          : entry.context_window_tokens;
            const auto model_variants = MaybeVariants(model);
            if (const nlohmann::json* declared = model.Raw("metadata")) {
                if (!declared->is_object()) model.Fail("metadata", "must be an object");
                resolved.declared_metadata = *declared;
            }
            model.Finish();
            resolved.variants_declared = model_variants.has_value() || provider_variants.has_value();
            // Effort variants: the model's own, else the provider's. Each
            // becomes a SIBLING ResolvedModel -- same connection and wire id,
            // parameters overlaid -- addressable as <provider>/<model>@<name>.
            // Synthesized BEFORE the move below: a sibling is a copy of `resolved`.
            const auto& variants = model_variants ? *model_variants
                                                  : provider_variants.value_or(
                                                        std::vector<ModelVariant>{});
            for (const auto& variant : variants) {
                ResolvedModel sibling = MakeVariantResolved(resolved, variant, variant_params_hook,
                                                            resolved.declared_metadata);
                const ModelRef sibling_key = sibling.ref;
                config.Add(sibling_key, std::move(sibling));
            }
            const ModelRef key = resolved.ref;
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
        throw ConfigError(std::string("providers.json: ") + error.what());
    }
}

ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path,
                                    const VariantParamsHook& variant_params_hook,
                                    const ProviderConfigExtensions* extensions) {
    // LoadJsonConfig's parse callback is single-argument (const json&); the
    // hook rides a capture instead of a second callback parameter.
    return LoadJsonConfig<ProvidersConfig>(
        path, "providers.json", AbsencePolicy::kThrow, EmptyFilePolicy::kTreatAsMalformed,
        [&variant_params_hook, extensions](const nlohmann::json& doc) {
            return ParseProvidersConfig(doc, variant_params_hook, extensions);
        });
}

}  // namespace minimal::config
