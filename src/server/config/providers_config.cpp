#include "minimal/config/providers_config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
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
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        return LookupKeyInAuthJson(nlohmann::json::parse(in), provider_name);
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
        return value.template get<T>();
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> OwnNumber(const nlohmann::json& node, const char* key) {
    if (node.contains(key) && node.at(key).is_number()) return node.at(key).get<T>();
    return std::nullopt;
}

// opencode-compat normalization, applied to each provider entry BEFORE the
// strict parse: an opencode-shaped entry is mapped onto minimal's fields in
// place, and native minimal keys always win when both are present.
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

}  // namespace

ProvidersConfig& ProvidersConfig::operator=(ProvidersConfig&& other) noexcept {
    std::lock_guard<std::mutex> a(cache_.mutex_);
    std::lock_guard<std::mutex> b(other.cache_.mutex_);
    catalog_.models_ = std::move(other.catalog_.models_);
    catalog_.providers_ = std::move(other.catalog_.providers_);
    cache_.gates_ = std::move(other.cache_.gates_);
    cache_.hook_ = std::move(other.cache_.hook_);
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

const ProviderEntry* ProvidersConfig::FindProviderLocked(const std::string& name) const {
    const auto it = catalog_.providers_.find(name);
    return it == catalog_.providers_.end() ? nullptr : &it->second;
}

void ProvidersConfig::SetDiscoveryHook(DiscoveryHook hook) {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    cache_.hook_ = std::move(hook);
}

std::shared_ptr<const ResolvedModel> ProvidersConfig::Resolve(const ModelRef& ref) const {
    const std::string key = ref.str();
    std::unique_lock<std::mutex> lock(cache_.mutex_);

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
            return it->second;
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
    if (!cache.attempted || cache.generation != cache_.generation_) {
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
        if (discovered != cache.models.end() &&
            (discovered->second.context_window_tokens || discovered->second.max_tokens)) {
            ResolvedModel copy = *static_it->second;
            if (!copy.context_window_tokens && discovered->second.context_window_tokens) {
                copy.context_window_tokens = discovered->second.context_window_tokens;
            }
            if (!copy.max_tokens && discovered->second.max_tokens) {
                copy.max_tokens = discovered->second.max_tokens;
            }
            return cache_.overlay_
                .emplace(ref_key, std::make_shared<const ResolvedModel>(std::move(copy)))
                .first->second;
        }
        return static_it->second;  // nothing to enrich
    }
    if (const auto it = cache_.overlay_.find(ref_key); it != cache_.overlay_.end()) return it->second;
    return nullptr;  // discovery succeeded; the backend does not serve this id
}

DiscoveryOutcome ProvidersConfig::FetchOutcome(const ProviderEntry& entry) const {
    DiscoveryOutcome outcome;
    // Snapshot the hook under cache_.mutex_ (lock order gate -> mutex_, per
    // DiscoveryCacheState) rather than reading it bare: SetDiscoveryHook
    // writes it under that mutex, and a bare read of a std::function mid-assign
    // is UB, however unlikely the overlap. The copy is released before the
    // hook runs -- the network I/O must not hold any config lock.
    DiscoveryHook hook;
    {
        std::lock_guard<std::mutex> lock(cache_.mutex_);
        hook = cache_.hook_;
    }
    if (!hook) {
        // Uninjected (a test or an unhosted config): report rather than treat
        // as success-with-zero-models, which would read as "backend serves
        // nothing".
        outcome.catalog_present = true;
        outcome.error = "no discovery hook set";
        return outcome;
    }
    return hook(entry.backend, entry.connection);
}

void ProvidersConfig::PublishLocked(const ProviderEntry& entry, const DiscoveryOutcome& outcome,
                                    DiscoveryCacheState::DiscoveryCache& cache) const {
    cache.attempted = true;
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
    }
}

std::vector<std::string> ProvidersConfig::ModelRefs() const {
    std::lock_guard<std::mutex> lock(cache_.mutex_);
    // Static ∪ cached-discovered. Both maps are individually sorted, but a
    // concatenation interleaves wrongly ("local/zdisc" sorts before
    // "mixed/static1" yet lives in the other map) -- so merge, skipping refs a
    // static entry already provides (an enriched static model lives in the
    // overlay too). A cold cache contributes nothing: this is a listing, never
    // a discovery trigger.
    std::vector<std::string> out;
    out.reserve(catalog_.models_.size() + cache_.overlay_.size());
    for (const auto& [key, unused] : catalog_.models_) out.push_back(key);
    for (const auto& [key, unused] : cache_.overlay_) {
        if (!catalog_.models_.count(key)) out.push_back(key);
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
                                     const std::string model = key.substr(slash + 1);
                                     if (!entry->second.whitelist.empty() &&
                                         std::find(entry->second.whitelist.begin(),
                                                   entry->second.whitelist.end(),
                                                   model) == entry->second.whitelist.end()) {
                                         return true;
                                     }
                                     return std::find(entry->second.blacklist.begin(),
                                                      entry->second.blacklist.end(),
                                                      model) != entry->second.blacklist.end();
                                 }),
                    out.end());
    }

    std::sort(out.begin(), out.end());
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

ProvidersConfig ParseProvidersConfigUnwrapped(const nlohmann::json& doc) {
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
        if (provider_normalized.contains("parameters") &&
            provider_normalized.at("parameters").is_object()) {
            entry.parameters = provider_normalized.at("parameters");
        }
        entry.max_tokens = OwnNumber<int>(provider_normalized, "max_tokens");
        entry.context_window_tokens =
            OwnNumber<std::int64_t>(provider_normalized, "context_window_tokens");
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
        if (provider_normalized.contains("headers") && provider_normalized.at("headers").is_array()) {
            for (const auto& pair : provider_normalized.at("headers")) {
                if (pair.is_array() && pair.size() == 2 && pair[0].is_string() &&
                    pair[1].is_string()) {
                    entry.connection.headers.emplace_back(pair[0].get<std::string>(),
                                                          pair[1].get<std::string>());
                }
            }
        }
        for (const char* key : {"whitelist", "blacklist"}) {
            if (provider_normalized.contains(key) && provider_normalized.at(key).is_array()) {
                for (const auto& id : provider_normalized.at(key)) {
                    if (id.is_string()) (key[0] == 'w' ? entry.whitelist : entry.blacklist)
                                           .push_back(id.get<std::string>());
                }
            }
        }
        config.AddProvider(entry);

        if (!has_models) continue;  // discover-only entry: nothing static to resolve

        for (const auto& [model_name, model] : provider_normalized.at("models").items()) {
            const std::string model_where = "model '" + provider_name + "/" + model_name + "'";
            if (!model.is_object()) {
                throw std::runtime_error("providers.json: " + model_where + " must be an object");
            }
            if (provider_name.find('/') != std::string::npos) {
                // Otherwise the reference cannot be parsed back apart.
                throw std::runtime_error("providers.json: provider '" + provider_name +
                                         "' contains a '/', which makes the reference ambiguous");
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
            resolved.parameters = InheritParameters(provider, model);
            resolved.max_tokens = InheritNumber<int>(provider, model, "max_tokens", model_where);
            resolved.context_window_tokens =
                InheritNumber<std::int64_t>(provider, model, "context_window_tokens",
                                            model_where);
            // The key is copied out FIRST. Passing `resolved.ref` alongside
            // `std::move(resolved)` in one call is unsequenced -- the move may
            // run first, and the key is then read from a moved-from string.
            const ModelRef key = resolved.ref;
            config.Add(key, std::move(resolved));
        }
    }
    return config;
}

ProvidersConfig ParseProvidersConfig(const nlohmann::json& doc) {
    // ParseProvidersConfigUnwrapped already throws well-formed
    // "providers.json: ..." runtime_errors for every schema violation it
    // anticipates (checked with contains()/is_*() before every get<T>()); this
    // only catches a stray nlohmann::json::exception a future unguarded get<T>()
    // might leak, normalizing it the same way settings/permissions self-wrap.
    try {
        return ParseProvidersConfigUnwrapped(doc);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("providers.json: ") + error.what());
    }
}

ProvidersConfig LoadProvidersConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<ProvidersConfig>(path, "providers", AbsencePolicy::kThrow,
                                           EmptyFilePolicy::kTreatAsMalformed, ParseProvidersConfig);
}

}  // namespace minimal::config
