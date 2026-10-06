#include "minimal/plugins/plugin_config_endpoint.hpp"

#include <stdexcept>

#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_store.hpp"

namespace minimal::plugins {
namespace {

using nlohmann::json;

std::string Need(const json& request, const char* key) {
    if (!request.is_object() || !request.contains(key) || !request.at(key).is_string()) {
        throw std::invalid_argument(std::string("'") + key + "' (a string) is required");
    }
    return request.at(key).get<std::string>();
}

}  // namespace

PluginConfigEndpoint::PluginConfigEndpoint(std::filesystem::path config_dir,
                                           const std::vector<PluginRecord>& records,
                                           const std::vector<PluginCandidate>& candidates)
    : config_dir_(std::move(config_dir)) {
    for (const auto& record : records) {
        Entry entry;
        entry.name = record.file.stem().string();
        entry.enabled = record.status != LoadStatus::Disabled;
        entry.loaded = record.ok();
        entry.symbols = record.config;
        // A disabled plugin was never opened, so the loader resolved nothing:
        // open it for its config symbols alone, so it can be configured
        // before it is enabled. (Its reconfigure is never called: it is not
        // running.)
        if (entry.symbols.schema == nullptr && record.status == LoadStatus::Disabled) {
            const auto inspected = InspectPluginConfig(record.file);
            if (inspected.error.empty()) entry.symbols = inspected.config;
        }
        if (entry.symbols.schema != nullptr) {
            try {
                entry.symbols.schema(entry.schema);
            } catch (...) {
                entry.schema = nullptr;  // a broken describe: not configurable here
            }
        }
        entry.applied = json::object();
        for (const auto& candidate : candidates) {
            if (candidate.file == record.file) entry.applied = candidate.config;
        }
        entries_[entry.name] = std::move(entry);
    }
}

PluginConfigEndpoint::Entry& PluginConfigEndpoint::Require(const json& request) {
    const std::string name = Need(request, "name");
    const auto it = entries_.find(name);
    if (it == entries_.end()) throw std::invalid_argument("no plugin '" + name + "'");
    if (it->second.schema.is_null()) {
        throw std::invalid_argument("plugin '" + name + "' has no configuration");
    }
    return it->second;
}

std::filesystem::path PluginConfigEndpoint::PathOf(const Entry& entry) const {
    return config_dir_ / common::kPluginConfigDirName / (entry.name + ".json");
}

json PluginConfigEndpoint::ReadFile(const Entry& entry) const {
    return common::ReadPluginConfigFile(
        PathOf(entry), common::PluginConfigLabel(common::kPluginConfigDirName, entry.name));
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
            {"file", common::PluginConfigLabel(common::kPluginConfigDirName, entry.name)},
            {"enabled", entry.enabled},
            {"loaded", entry.loaded},
            {"schema", entry.schema},
            {"config", file},
            {"effective", std::move(effective)},
            {"unused", std::move(unused)},
            {"pending", Pending(entry, file)}};
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
        // Recorded as applied: nothing about these differs for the plugin.
        entry.applied = file;
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
    std::string error;
    if (entry.symbols.validate != nullptr) entry.symbols.validate(next, error);
    if (!error.empty()) return {{"error", error}};
    if (const std::string failed = common::WritePluginConfigFile(PathOf(entry), next);
        !failed.empty()) {
        return {{"error", failed}};
    }
    return Apply(entry, next);
}

std::string PluginConfigEndpoint::Call(const std::string& method, const std::string& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        const json request = payload.empty() ? json::object() : json::parse(payload);
        if (method == "list") {
            json rows = json::array();
            for (const auto& [name, entry] : entries_) {
                json row = {{"name", name},
                            {"enabled", entry.enabled},
                            {"loaded", entry.loaded},
                            {"configurable", !entry.schema.is_null()},
                            {"set", 0},
                            {"pending", json::array()}};
                if (!entry.schema.is_null()) {
                    try {
                        const json file = ReadFile(entry);
                        row["set"] = file.empty() ? 0 : file.flatten().size();
                        row["pending"] = Pending(entry, file);
                    } catch (const std::exception& e) {
                        row["error"] = e.what();
                    }
                }
                rows.push_back(std::move(row));
            }
            return rows.dump();
        }
        if (method == "describe") return Describe(Require(request)).dump();
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
            return Apply(entry, ReadFile(entry)).dump();
        }
        return json{{"error", "unknown method '" + method +
                                  "' (known: list, describe, set, unset, apply)"}}
            .dump();
    } catch (const std::exception& error) {
        return json{{"error", error.what()}}.dump();
    }
}

}  // namespace minimal::plugins
