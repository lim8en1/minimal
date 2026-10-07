#include "minimal/plugins/plugin_config_endpoint.hpp"

#include <algorithm>
#include <stdexcept>

#include "minimal/common/atomic_write.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_store.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/policy.hpp"

namespace minimal::plugins {
namespace {

using nlohmann::json;

std::string Need(const json& request, const char* key) {
    if (!request.is_object() || !request.contains(key) || !request.at(key).is_string()) {
        throw std::invalid_argument(std::string("'") + key + "' (a string) is required");
    }
    return request.at(key).get<std::string>();
}

// What a plugin registered: {label: [distinct names]} ("provider": ["openai"]).
json ProvidesOf(const PluginRecord& record) {
    json out = json::object();
    for (const auto& [label, names] : record.registered) {
        json distinct = json::array();
        for (const auto& name : names) {
            if (std::find(distinct.begin(), distinct.end(), name) == distinct.end()) {
                distinct.push_back(name);
            }
        }
        if (!distinct.empty()) out[label] = std::move(distinct);
    }
    return out;
}

}  // namespace

PluginConfigEndpoint::PluginConfigEndpoint(std::filesystem::path config_dir,
                                           const std::vector<PluginRecord>& records,
                                           const std::vector<PluginCandidate>& candidates,
                                           BackendsInUse backends_in_use,
                                           const ServiceRegistry* services,
                                           std::vector<CoreConfigFile> core_files)
    : config_dir_(std::move(config_dir)),
      backends_in_use_(std::move(backends_in_use)),
      services_(services),
      check_thread_([this] { CheckLoop(); }) {
    for (auto& core : core_files) {
        Entry entry;
        entry.name = core.name;
        entry.core = true;
        entry.path = std::move(core.path);
        entry.label = core.name;
        entry.enabled = true;
        entry.loaded = true;
        entry.schema = std::move(core.schema);
        entry.validate = std::move(core.validate);
        entry.plugin_choices = std::move(core.plugin_choices);
        try {
            entry.applied = ReadFile(entry);
        } catch (const std::exception&) {
            entry.applied = json::object();   // the server started; an edit since is what broke it
        }
        entries_[entry.name] = std::move(entry);
    }
    for (const auto& record : records) {
        Entry entry;
        entry.name = record.file.stem().string();
        entry.file = record.file.filename().string();
        entry.path = config_dir_ / common::kPluginConfigDirName / (entry.name + ".json");
        entry.label = common::PluginConfigLabel(common::kPluginConfigDirName, entry.name);
        entry.provenance = record.provenance;
        entry.provider_kinds = record.Registered("provider");
        entry.provides = ProvidesOf(record);
        entry.enabled = record.status != LoadStatus::Disabled;
        entry.loaded = record.ok();
        entry.symbols = record.config;
        // A disabled plugin was never opened, so the loader resolved nothing.
        // It is opened for its config symbols alone -- but only when a
        // request names it (Find), never at startup: "disabled" is the escape
        // hatch for a plugin whose loading misbehaves, and opening it runs
        // its static initializers inside the server.
        if (entry.symbols.schema == nullptr && record.status == LoadStatus::Disabled) {
            entry.inspected = false;
            entry.module = record.file;
        }
        AdoptSymbols(entry);
        entry.applied = json::object();
        for (const auto& candidate : candidates) {
            if (candidate.file == record.file) entry.applied = candidate.config;
        }
        entries_.emplace(entry.name, std::move(entry));   // a core file's name stays the core file
    }
}

PluginConfigEndpoint::~PluginConfigEndpoint() { Stop(); }

void PluginConfigEndpoint::Stop() {
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        stopping_ = true;
        check_queue_.clear();
    }
    checks_cv_.notify_all();
    if (check_thread_.joinable()) check_thread_.join();
}

void PluginConfigEndpoint::AdoptSymbols(Entry& entry) {
    if (entry.symbols.schema != nullptr) {
        try {
            entry.symbols.schema(entry.schema);
        } catch (...) {
            entry.schema = nullptr;  // a broken describe: not configurable here
        }
    }
    if (const auto validate = entry.symbols.validate; validate != nullptr) {
        entry.validate = [validate](const json& next) {
            std::string error;
            validate(next, error);
            return error;
        };
    }
}

void PluginConfigEndpoint::Inspect(Entry& entry) {
    if (entry.inspected) return;
    entry.inspected = true;
    const auto inspected = InspectPluginConfig(entry.module);
    if (!inspected.error.empty()) return;
    entry.symbols = inspected.config;
    AdoptSymbols(entry);
}

void PluginConfigEndpoint::CheckLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(checks_mutex_);
            checks_cv_.wait(lock, [this] { return stopping_ || !check_queue_.empty(); });
            if (stopping_) return;
            task = std::move(check_queue_.front());
            check_queue_.pop_front();
        }
        task();
    }
}

void PluginConfigEndpoint::StartCheck(const Entry& entry, const json& file, bool on_demand) {
    if (entry.symbols.check == nullptr) return;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        CheckState& state = checks_[entry.name];
        generation = ++state.generation;
        state.state = "running";
        check_queue_.push_back([this, name = entry.name, check = entry.symbols.check,
                                services = entry.loaded ? services_ : nullptr, file, generation,
                                on_demand] {
            ConfigCheckContext context;
            context.services = services;
            context.budget = kConfigCheckBudget;
            context.on_demand = on_demand;
            // Counted from when the check STARTS, not when it was queued.
            context.deadline = std::chrono::steady_clock::now() + kConfigCheckBudget;
            context.cancelled = [this, name, generation, deadline = context.deadline] {
                if (std::chrono::steady_clock::now() >= deadline) return true;
                std::lock_guard<std::mutex> lock(checks_mutex_);
                return stopping_ || checks_[name].generation != generation;
            };
            json out = json::array();
            try {
                check(file, context, out);
                if (!out.is_array()) out = json::array();
            } catch (const std::exception& error) {
                out = json::array({{{"path", ""}, {"level", "error"},
                                    {"message", std::string("the check failed: ") + error.what()}}});
            } catch (...) {
                out = json::array({{{"path", ""}, {"level", "error"}, {"message", "the check failed"}}});
            }
            std::lock_guard<std::mutex> lock(checks_mutex_);
            CheckState& state = checks_[name];
            if (state.generation != generation) return;   // a newer check superseded it
            state.state = "done";
            state.results = std::move(out);
            state.at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        });
    }
    checks_cv_.notify_one();
}

json PluginConfigEndpoint::ChecksOf(const Entry& entry) const {
    if (entry.symbols.check == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(checks_mutex_);
    const auto it = checks_.find(entry.name);
    if (it == checks_.end()) return {{"state", "idle"}, {"results", json::array()}};
    return {{"state", it->second.state},
            {"at", it->second.at_unix_ms},
            {"results", it->second.results}};
}

PluginConfigEndpoint::Entry& PluginConfigEndpoint::Find(const json& request) {
    const std::string name = Need(request, "name");
    const auto it = entries_.find(name);
    if (it == entries_.end()) throw std::invalid_argument("no plugin '" + name + "'");
    Inspect(it->second);
    return it->second;
}

PluginConfigEndpoint::Entry& PluginConfigEndpoint::Require(const json& request) {
    Entry& entry = Find(request);
    if (entry.schema.is_null()) {
        throw std::invalid_argument("plugin '" + entry.name + "' has no configuration");
    }
    return entry;
}

std::map<std::string, bool> PluginConfigEndpoint::EnabledInFile() const {
    std::map<std::string, bool> out;
    PluginConfig file;
    try {
        file = ParsePluginConfig(common::ReadFileOrEmpty(config_dir_ / "plugins.json"));
    } catch (const std::exception&) {
        return out;
    }
    for (const auto& [name, entry] : entries_) {
        if (entry.core) continue;
        for (const auto& row : file.plugins) {
            if (row.file != entry.file) continue;
            if (row.provenance && *row.provenance != entry.provenance) continue;
            if (row.enabled) out[name] = *row.enabled;
        }
    }
    return out;
}

json PluginConfigEndpoint::SetEnabled(Entry& entry, bool enabled) {
    if (entry.core) return {{"error", entry.name + " is core configuration, not a plugin"}};
    const auto path = config_dir_ / "plugins.json";
    const std::string before = common::ReadFileOrEmpty(path);
    PluginConfig file = ParsePluginConfig(before);
    if (file.malformed || !file.problems.empty()) {
        return {{"error", "plugins.json cannot be used as written; fix it first" +
                              (file.problems.empty() ? std::string() : ": " + file.problems.front())}};
    }
    if (!enabled && !entry.provider_kinds.empty() && backends_in_use_) {
        // What every OTHER plugin enabled in the file (and running, so its
        // registrations are known) still provides.
        const auto in_file = EnabledInFile();
        std::set<std::string> remaining;
        for (const auto& [name, other] : entries_) {
            if (&other == &entry || !other.loaded || other.core) continue;
            const auto it = in_file.find(name);
            if (it != in_file.end() && !it->second) continue;
            remaining.insert(other.provider_kinds.begin(), other.provider_kinds.end());
        }
        const std::set<std::string> used = backends_in_use_();
        bool usable = used.empty();
        std::string lost;
        for (const auto& backend : used) {
            if (remaining.count(backend) != 0) usable = true;
            lost += (lost.empty() ? "" : ", ") + backend;
        }
        if (!usable) {
            return {{"error", "disabling " + entry.name + " would leave no provider for "
                              "providers.json's models (they use: " + lost +
                              ") -- the server would not start"}};
        }
    }
    bool found = false;
    for (auto& row : file.plugins) {
        if (row.file != entry.file) continue;
        if (row.provenance && *row.provenance != entry.provenance) continue;
        row.provenance = entry.provenance;
        row.enabled = enabled;
        found = true;
    }
    if (!found) {
        file.plugins.push_back({entry.file, entry.provenance, enabled});
    }
    const auto outcome = common::write_file(path, SerializePluginConfig(file));
    if (!outcome.ok) return {{"error", "could not write plugins.json: " + outcome.error}};
    const bool now = enabled == entry.enabled;
    return {{"applied", now ? "none" : "restart"},
            {"enabled", enabled},
            {"reason", now ? "already " + std::string(enabled ? "running" : "off")
                           : "applied when the server restarts"}};
}

json PluginConfigEndpoint::ReadFile(const Entry& entry) const {
    return common::ReadPluginConfigFile(entry.path, entry.label);
}

std::vector<std::string> PluginConfigEndpoint::PluginsRegisteredUnder(const std::string& label) const {
    std::vector<std::string> out;
    for (const auto& [name, entry] : entries_) {
        if (!entry.core && entry.provides.is_object() && entry.provides.contains(label)) {
            out.push_back(name);
        }
    }
    return out;
}

json PluginConfigEndpoint::SchemaOf(const Entry& entry, const json& file) const {
    json schema = entry.schema;
    if (entry.plugin_choices.empty() || !schema.contains("fields")) return schema;
    for (auto& field : schema["fields"]) {
        const auto it = entry.plugin_choices.find(field.value("path", ""));
        if (it == entry.plugin_choices.end()) continue;
        json choices = json::array();
        for (const auto& name : PluginsRegisteredUnder(it->second)) choices.push_back(name);
        // The file's own value stays choosable: a disabled or misspelled
        // plugin still shows as what the file says.
        if (const json* current = common::ConfigGet(file, it->first);
            current != nullptr && current->is_string() &&
            std::find(choices.begin(), choices.end(), *current) == choices.end()) {
            choices.push_back(*current);
        }
        field["type"] = "enum";
        field["choices"] = std::move(choices);
    }
    return schema;
}

json PluginConfigEndpoint::UnselectedOf(const Entry& entry) const {
    json out = json::array();
    if (entry.core || !entry.provides.is_object()) return out;
    for (const auto& [name, core] : entries_) {
        if (!core.core) continue;
        for (const auto& [field, label] : core.plugin_choices) {
            if (!entry.provides.contains(label)) continue;
            json file;
            try {
                file = ReadFile(core);
            } catch (const std::exception&) {
                continue;   // the core row reports the broken file
            }
            const json* value = common::ConfigGet(file, field);
            if (value == nullptr || !value->is_string() || value->get<std::string>() != entry.name) {
                out.push_back({{"file", core.name}, {"field", field}});
            }
        }
    }
    return out;
}

json PluginConfigEndpoint::Describe(const Entry& entry) const {
    const json file = ReadFile(entry);
    json effective = json::object();
    json unused = json::object();
    for (const auto& row : common::EffectiveConfig(entry.schema, file)) {
        effective[row.path] = row.value;
        if (row.unused) unused[row.path] = *row.unused;
    }
    return {{"name", entry.name},
            {"kind", entry.core ? "core" : "plugin"},
            {"file", entry.label},
            {"enabled", entry.enabled},
            {"loaded", entry.loaded},
            {"schema", SchemaOf(entry, file)},
            {"config", file},
            {"effective", std::move(effective)},
            {"unused", std::move(unused)},
            {"pending", Pending(entry, file)},
            {"checks", ChecksOf(entry)}};
}

json PluginConfigEndpoint::Pending(const Entry& entry, const json& file) const {
    // A plugin that is not running (disabled, or it failed to load) has
    // nothing pending: it reads its file whenever it does start. Diffing it
    // against the {} it never ran with reported every set field as "restart
    // pending" through any number of restarts.
    if (!entry.loaded) return json::array();
    return common::DiffConfig(entry.schema, entry.applied, file).paths;
}

json PluginConfigEndpoint::Apply(Entry& entry, const json& file) {
    if (!entry.loaded) {
        return {{"applied", "not_running"},
                {"reason", entry.enabled ? "the plugin failed to load" : "the plugin is disabled"}};
    }
    const auto change = common::DiffConfig(entry.schema, entry.applied, file);
    if (change.paths.empty()) {
        // Nothing the running plugin uses changed. A field saved while its
        // condition is unmet takes effect when it is met.
        json unused = json::object();
        for (const auto& path : change.unused) {
            if (const json* field = common::SchemaField(entry.schema, path)) {
                unused[path] = common::UnusedReason(entry.schema, *field, file).value_or("");
            }
        }
        // NOT recorded as applied: a restart-only field saved while unused
        // was never applied, and once a live condition field makes it used
        // the diff must still show it (else "live" would skip a restart it
        // needs). Pending already ignores fields unused on both sides.
        return {{"applied", "none"}, {"changed", json::array()}, {"unused", std::move(unused)}};
    }
    // Live only when the plugin is running, every changed field is live, and
    // it takes the new config; anything else waits for a restart.
    if (entry.loaded && change.all_live && entry.symbols.reconfigure != nullptr) {
        std::string error;
        bool ok = false;
        try {
            ok = entry.symbols.reconfigure(file, error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (ok) {
            entry.applied = file;
            return {{"applied", "live"}, {"changed", change.paths}};
        }
        return {{"applied", "restart"}, {"changed", change.paths}, {"reconfigure_error", error}};
    }
    return {{"applied", "restart"}, {"changed", change.paths}};
}

json PluginConfigEndpoint::Save(Entry& entry, const json& next) {
    if (entry.validate) {
        if (const std::string error = entry.validate(next); !error.empty()) return {{"error", error}};
    }
    // A core file is never removed: the server does not start without it.
    if (const std::string failed = common::WritePluginConfigFile(
            entry.path, next,
            entry.core ? common::EmptyConfig::kKeepFile : common::EmptyConfig::kRemoveFile);
        !failed.empty()) {
        return {{"error", failed}};
    }
    json applied = Apply(entry, next);
    StartCheck(entry, next);   // what was saved, against the world
    return applied;
}

std::string PluginConfigEndpoint::Call(const std::string& method, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        const json request = payload.empty() ? json::object() : json::parse(payload);
        if (method == "list") {
            json rows = json::array();
            const auto in_file = EnabledInFile();
            for (const auto& [name, entry] : entries_) {
                const auto file_enabled = in_file.find(name);
                json row = {{"name", name},
                            {"kind", entry.core ? "core" : "plugin"},
                            {"enabled", entry.enabled},
                            {"enabled_in_file", file_enabled != in_file.end()
                                                    ? file_enabled->second
                                                    : entry.enabled},
                            {"loaded", entry.loaded},
                            // A disabled plugin not yet opened (see
                            // Inspect) is listed as configurable: naming
                            // it opens it, and describe says if it is not.
                            {"configurable", !entry.inspected || !entry.schema.is_null()},
                            {"provides", entry.provides},
                            {"set", 0},
                            {"pending", json::array()}};
                if (json unselected = UnselectedOf(entry); !unselected.empty()) {
                    row["unselected"] = std::move(unselected);
                }
                if (!entry.schema.is_null()) {
                    try {
                        const json file = ReadFile(entry);
                        row["set"] = file.empty() ? 0 : file.flatten().size();
                        row["pending"] = Pending(entry, file);
                        // The last check, counted: the list says what needs a look.
                        if (const json checks = ChecksOf(entry); !checks.is_null()) {
                            int errors = 0;
                            int warnings = 0;
                            for (const auto& result : checks.value("results", json::array())) {
                                const std::string level = result.value("level", "");
                                errors += level == "error" ? 1 : 0;
                                warnings += level == "warning" ? 1 : 0;
                            }
                            row["checks"] = {{"state", checks.value("state", "")},
                                             {"errors", errors},
                                             {"warnings", warnings}};
                        }
                    } catch (const std::exception& e) {
                        row["error"] = e.what();
                    }
                }
                rows.push_back(std::move(row));
            }
            return rows.dump();
        }
        if (method == "describe") return Describe(Require(request)).dump();
        if (method == "enable") {
            if (!request.contains("enabled") || !request.at("enabled").is_boolean()) {
                throw std::invalid_argument("'enabled' (true or false) is required");
            }
            return SetEnabled(Find(request), request.at("enabled").get<bool>()).dump();
        }
        if (method == "set" || method == "unset") {
            Entry& entry = Require(request);
            const std::string key = Need(request, "key");
            const json* field = common::SchemaField(entry.schema, key);
            if (field == nullptr) throw std::invalid_argument("unknown key '" + key + "'");
            json next = ReadFile(entry);
            if (method == "unset") {
                common::ConfigUnset(next, key);
            } else if (request.contains("text")) {
                common::ConfigSet(next, key, common::ParseFieldValue(*field, Need(request, "text")));
            } else if (request.contains("value")) {
                common::ConfigSet(next, key, request.at("value"));
            } else {
                throw std::invalid_argument("'value' or 'text' is required");
            }
            return Save(entry, next).dump();
        }
        if (method == "apply") {
            Entry& entry = Require(request);
            const json file = ReadFile(entry);
            json applied = Apply(entry, file);
            StartCheck(entry, file);
            return applied.dump();
        }
        if (method == "check") {
            Entry& entry = Require(request);
            if (entry.symbols.check == nullptr) {
                return json{{"error", entry.name + " has no config check"}}.dump();
            }
            StartCheck(entry, ReadFile(entry), /*on_demand=*/true);
            return json{{"started", true}}.dump();
        }
        return json{{"error", "unknown method '" + method +
                                  "' (known: list, describe, set, unset, apply, check, enable)"}}
            .dump();
    } catch (const std::exception& error) {
        return json{{"error", error.what()}}.dump();
    }
}

}  // namespace minimal::plugins
