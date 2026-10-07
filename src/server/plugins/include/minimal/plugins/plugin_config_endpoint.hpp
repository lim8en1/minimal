#pragma once

// The `plugins` endpoint: plugin configuration for a running server, over
// the generic PluginCall channel (PluginCall("plugins", <method>, <json>)).
// Core provides it -- it needs every plugin's record and config symbols, and
// the file each plugin registered with -- under PluginEndpointServiceName
// ("plugins"), between LoadAll and Seal.
//
// Methods (payloads and answers are JSON):
//   list                      [{name, enabled, enabled_in_file, loaded, configurable,
//                              provides: {label: [names]}, set, pending}] -- every plugin;
//                              enabled_in_file differs from enabled after a
//                              toggle, until the restart that applies it
//   check {name}              runs the plugin's config check again (also run
//                             after every save): {started: true}, or an error
//                             for a plugin with no check
//   enable {name, enabled}    sets the plugin's plugins.json entry; applied at
//                             the next server start ({applied: "restart"}). A
//                             disable that would leave no usable provider for
//                             providers.json is refused, saying which
//   describe {name}           {name, file, enabled, loaded, schema, config,
//                              effective: {path: value}, pending: [paths],
//                              unused: {path: why}} -- fields whose "when" is
//                              unmet, so they do nothing right now
//   set {name, key, value}    or {name, key, text} (text parsed by the field's
//                             type); validated by the plugin, written, applied
//   unset {name, key}
//   apply {name}              re-read the file (after an edit elsewhere -- the
//                             CLI) and apply it
// set/unset/apply answer {applied: "live" | "restart" | "none", changed: [...]}
// -- "live" when every changed field is live and the plugin took it
// (minimal_plugin_reconfigure); "restart" when the change waits for one;
// "none" when nothing the plugin uses changed (with "unused": {path: why}
// for a field saved while its condition is unmet); "not_running" (with a
// "reason") for a disabled plugin or one that failed to load -- it reads the
// file when it starts.
// A refusal answers {error: "..."}.
//
// "pending" lists the fields whose file value differs from what the plugin
// is running with: the changes a restart would bring.
//
// "checks" (describe; a summary in list) is the plugin's last config check
// (minimal_plugin_check_config) -- {state: "running" | "done", at,
// results: [{path, level, message}]} -- or null for a plugin without one.
// Checks run on the endpoint's own thread, never inside a request: a slow
// probe holds up nothing, and a newer save cancels the check in flight.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <thread>
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
    // The backends providers.json's models use, read when asked (the file
    // may have changed since startup): what a disable must leave registered.
    using BackendsInUse = std::function<std::set<std::string>()>;

    // `records` and `candidates` from the same LoadAll, matched by file.
    // `services`: the host's, handed to a running plugin's config check.
    PluginConfigEndpoint(std::filesystem::path config_dir, const std::vector<PluginRecord>& records,
                         const std::vector<PluginCandidate>& candidates,
                         BackendsInUse backends_in_use = {},
                         const ServiceRegistry* services = nullptr);
    // Cancels the check in flight and waits for it.
    ~PluginConfigEndpoint() override;
    PluginConfigEndpoint(const PluginConfigEndpoint&) = delete;
    PluginConfigEndpoint& operator=(const PluginConfigEndpoint&) = delete;

    std::string Call(const std::string& method, const std::string& json_payload) override;
    bool Subscribe(const std::string&, const std::string&,
                   std::shared_ptr<PluginSubscriptionSink>) override {
        return false;
    }

private:
    struct Entry {
        std::string name;  // the module's stem: plugins.d/<name>.json
        std::string file;  // the module's basename, its plugins.json key
        Provenance provenance = Provenance::User;
        bool enabled = false;
        bool loaded = false;
        std::vector<std::string> provider_kinds;  // the backends it registered
        nlohmann::json provides;                  // {registry label: [names]}
        PluginConfigSymbols symbols;
        nlohmann::json schema;   // null: the plugin takes no configuration
        nlohmann::json applied;  // the config the plugin is running with
    };

    Entry& Require(const nlohmann::json& request);
    // The plugin by name, configurable or not.
    Entry& Find(const nlohmann::json& request);
    // plugins.json's enabled value for each plugin, as the file says now.
    std::map<std::string, bool> EnabledInFile() const;
    nlohmann::json SetEnabled(Entry& entry, bool enabled);

    // The plugin's last check, as describe/list show it (null: no check).
    struct CheckState {
        std::string state = "idle";
        std::uint64_t generation = 0;
        std::int64_t at_unix_ms = 0;
        nlohmann::json results = nlohmann::json::array();
    };
    // Queues a check of `file` for `entry`, cancelling the one in flight.
    // Cheap: called with mutex_ held, from the request that saved.
    void StartCheck(const Entry& entry, const nlohmann::json& file);
    nlohmann::json ChecksOf(const Entry& entry) const;
    void CheckLoop();
    std::filesystem::path PathOf(const Entry& entry) const;
    nlohmann::json ReadFile(const Entry& entry) const;
    nlohmann::json Describe(const Entry& entry) const;
    // Fields whose file value differs from what the running plugin uses;
    // none for a plugin that is not running.
    nlohmann::json Pending(const Entry& entry, const nlohmann::json& file) const;
    // Validates, writes and applies `next`.
    nlohmann::json Save(Entry& entry, const nlohmann::json& next);
    // Brings the running plugin to `file` where it can: {applied, changed}.
    nlohmann::json Apply(Entry& entry, const nlohmann::json& file);

    std::filesystem::path config_dir_;
    BackendsInUse backends_in_use_;
    const ServiceRegistry* services_;
    std::mutex mutex_;  // RPC threads call concurrently; one edit at a time
    std::map<std::string, Entry> entries_;

    // The checks: their own lock (never held with plugin code running), the
    // queue the thread below drains, and each plugin's last state.
    mutable std::mutex checks_mutex_;
    std::condition_variable checks_cv_;
    std::deque<std::function<void()>> check_queue_;
    std::map<std::string, CheckState> checks_;
    bool stopping_ = false;
    std::thread check_thread_;  // last: starts once everything above exists
};

}  // namespace minimal::plugins
