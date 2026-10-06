#pragma once

// The `plugins` endpoint: plugin configuration for a running server, over
// the generic PluginCall channel (PluginCall("plugins", <method>, <json>)).
// Core provides it -- it needs every plugin's record and config symbols, and
// the file each plugin registered with -- under PluginEndpointServiceName
// ("plugins"), between LoadAll and Seal.
//
// Methods (payloads and answers are JSON):
//   list                      [{name, enabled, loaded, configurable, set, pending}]
//   describe {name}           {name, file, enabled, loaded, schema, config,
//                              effective: {path: value}, pending: [paths],
//                              unused: {path: why}} -- fields whose "when" is
//                              unmet, so they do nothing right now
//   set {name, key, value}    or {name, key, text} (text parsed by the field's
//                             type); validated by the plugin, written, applied
//   unset {name, key}
//   apply {name}              re-read the file (after an edit elsewhere -- the
//                             CLI) and apply it
// set/unset/apply answer {applied: "live" | "restart" | "disabled" | "none",
// changed: [...]} -- "live" when every changed field is live and the plugin
// took it (minimal_plugin_reconfigure); "restart" when the change waits for
// one; "disabled" when the plugin is switched off (it takes the file as it is
// when enabled, so its "pending" is always empty); "none" when nothing the plugin uses changed (with "unused": {path: why}
// for a field saved while its condition is unmet).
// A refusal answers {error: "..."}.
//
// "pending" lists the fields whose file value differs from what the plugin
// is running with: the changes a restart would bring.

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/plugin_loader.hpp"

namespace minimal::plugins {

inline constexpr const char* kPluginConfigEndpointName = "plugins";

class PluginConfigEndpoint final : public PluginEndpoint {
public:
    // `records` and `candidates` from the same LoadAll, matched by file.
    PluginConfigEndpoint(std::filesystem::path config_dir, const std::vector<PluginRecord>& records,
                         const std::vector<PluginCandidate>& candidates);

    std::string Call(const std::string& method, const std::string& json_payload) override;
    bool Subscribe(const std::string&, const std::string&,
                   std::shared_ptr<PluginSubscriptionSink>) override {
        return false;
    }

private:
    struct Entry {
        std::string name;  // the module's stem: plugins.d/<name>.json
        bool enabled = false;
        bool loaded = false;
        PluginConfigSymbols symbols;
        nlohmann::json schema;   // null: the plugin takes no configuration
        nlohmann::json applied;  // the config the plugin is running with
    };

    Entry& Require(const nlohmann::json& request);
    // The paths whose file value the running plugin does not have yet.
    static nlohmann::json Pending(const Entry& entry, const nlohmann::json& file);
    std::filesystem::path PathOf(const Entry& entry) const;
    nlohmann::json ReadFile(const Entry& entry) const;
    nlohmann::json Describe(const Entry& entry) const;
    // Validates, writes and applies `next`.
    nlohmann::json Save(Entry& entry, const nlohmann::json& next);
    // Brings the running plugin to `file` where it can: {applied, changed}.
    nlohmann::json Apply(Entry& entry, const nlohmann::json& file);

    std::filesystem::path config_dir_;
    std::mutex mutex_;  // RPC threads call concurrently; one edit at a time
    std::map<std::string, Entry> entries_;
};

}  // namespace minimal::plugins
