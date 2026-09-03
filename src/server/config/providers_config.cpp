#include "minimal/config/providers_config.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "minimal/common/config_paths.hpp"
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
    const auto value = common::LookupEnv(name);
    if (!value) {
        throw std::runtime_error("providers.json: " + where + " names api_key_env '" + name +
                                 "' which is unset");
    }
    return *value;
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
        const auto& value = node->at(key);
        if (!value.is_number_integer()) {
            throw std::runtime_error("providers.json: " + where + " field \"" + key +
                                     "\" must be an integer, got " + value.dump());
        }
        return value.get<T>();
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> OwnNumber(const nlohmann::json& node, const char* key) {
    if (node.contains(key) && node.at(key).is_number()) return node.at(key).get<T>();
    return std::nullopt;
}

}  // namespace

namespace {

// The overlay ResolvedModel for one discovered model: the entry's connection
// and provider-level defaults, the backend's id and window. Touches neither
// ImmutableCatalog nor DiscoveryCacheState directly -- a pure function of its
// two parameters despite living beside methods that hold cache_.mutex_ -- so
// it needs no ProvidersConfig membership at all.
ResolvedModel BuildDiscoveredLocked(const ProviderEntry& entry,
                                    const llm::DiscoveredModel& discovered) {
    // The backend knows the model id it serves; the entry supplies the
    // connection and the provider-level defaults, inherited exactly as a
    // static model would inherit them. The backend's context window wins over
    // the provider-level default, the same precedence InheritNumber gives a
    // model's own value over the provider's.
    ResolvedModel resolved;
    resolved.ref = ModelRef{entry.name, discovered.id};
    resolved.backend = entry.backend;
    resolved.provider = entry.connection;
    resolved.provider.model_name = discovered.id;
    resolved.parameters = entry.parameters;
    resolved.max_tokens = entry.max_tokens;
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
        if (it->second->context_window_tokens) return it->second;
        // A static entry usually resolves as parsed. Under a discover-entry, a
        // model whose context_window_tokens is absent can still be enriched
        // from the catalog -- but only by handing back an overlay COPY: the
        // parsed node must never be mutated after a snapshot of it has escaped.
        if (const auto enriched = cache_.overlay_.find(key); enriched != cache_.overlay_.end()) {
            return enriched->second;
        }
        if (const ProviderEntry* entry = FindProviderLocked(ref.provider);
            entry != nullptr && entry->discover) {
            return ResolveDiscovered(*entry, ref.model, key, lock);
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
        // and the discovered window fills the gap that made Resolve consult
        // discovery at all.
        const auto discovered = cache.models.find(model_id);
        if (discovered != cache.models.end() && discovered->second.context_window_tokens) {
            ResolvedModel copy = *static_it->second;
            copy.context_window_tokens = discovered->second.context_window_tokens;
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
    if (!cache_.hook_) {
        // Uninjected (a test or an unhosted config): report rather than treat
        // as success-with-zero-models, which would read as "backend serves
        // nothing".
        outcome.catalog_present = true;
        outcome.error = "no discovery hook set";
        return outcome;
    }
    return cache_.hook_(entry.backend, entry.connection);
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
    // ModelRefs() and /providers, and it means a later Resolve() of any of
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

        const std::string backend = RequireString(provider, "backend", where);
        const std::string url = RequireString(provider, "url", where);
        const std::string api_key = ResolveApiKey(provider, where);

        bool discover = false;
        if (provider.contains("discover")) {
            if (!provider.at("discover").is_boolean()) {
                throw std::runtime_error("providers.json: " + where +
                                         " sets \"discover\" which must be a boolean");
            }
            discover = provider.at("discover").get<bool>();
        }

        const bool has_models = provider.contains("models") && provider.at("models").is_object() &&
                                !provider.at("models").empty();
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
        if (provider.contains("parameters") && provider.at("parameters").is_object()) {
            entry.parameters = provider.at("parameters");
        }
        entry.max_tokens = OwnNumber<int>(provider, "max_tokens");
        entry.context_window_tokens = OwnNumber<std::int64_t>(provider, "context_window_tokens");
        config.AddProvider(entry);

        if (!has_models) continue;  // discover-only entry: nothing static to resolve

        for (const auto& [model_name, model] : provider.at("models").items()) {
            const std::string model_where = "model '" + provider_name + "/" + model_name + "'";
            if (!model.is_object()) {
                throw std::runtime_error("providers.json: " + model_where + " must be an object");
            }
            if (model_name.find('/') != std::string::npos ||
                provider_name.find('/') != std::string::npos) {
                // Otherwise the reference cannot be parsed back apart.
                throw std::runtime_error("providers.json: '" + provider_name + "/" + model_name +
                                         "' contains a '/' in a name, which makes the reference "
                                         "ambiguous");
            }

            ResolvedModel resolved;
            resolved.ref = ModelRef{provider_name, model_name};
            resolved.backend = backend;
            resolved.provider.base_url = url;
            resolved.provider.api_key = api_key;
            resolved.provider.model_name = model_name;
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
