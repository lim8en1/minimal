#pragma once

// Plugin extensions to config parsing. Core parses only its own shapes; a
// plugin that reads another tool's config (the opencode-compat plugin:
// opencode's provider blocks and its auth.json) does so here, so core never
// names that tool.
//
//   - A ProviderEntryTranslator rewrites one providers.json entry into core's
//     shape BEFORE core parses it. Translators run in registration order; each
//     sees the previous one's output. Throw to reject the entry (a config
//     error, naming `where`).
//   - A SecretSource (secrets.hpp) answers "secret:<plugin>:NAME" references.
//     Its namespace is the registering plugin's name, taken from the
//     registration attribution -- a plugin cannot answer for another's.
//
// Filled once, during plugin load (minimal_plugin_register_provider_config_
// extensions), before providers.json is read; read-only afterwards.

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/config/secrets.hpp"

namespace minimal::config {

class ProviderEntryTranslator {
public:
    virtual ~ProviderEntryTranslator() = default;
    virtual void Translate(nlohmann::json& entry, const std::string& where) const = 0;
};

class ProviderConfigExtensions {
public:
    void AddTranslator(std::shared_ptr<const ProviderEntryTranslator> translator);
    // One source per plugin, under the plugin's name; a second is ignored with
    // a warning, as is one registered outside a plugin's registration window.
    void AddSecretSource(std::shared_ptr<const SecretSource> source);

    // Applies every translator to `entry`, in registration order.
    void Translate(nlohmann::json& entry, const std::string& where) const;
    // The source registered by `plugin_name`, or null.
    const SecretSource* FindSecretSource(const std::string& plugin_name) const;

    // Plugin-loader bookkeeping (the registration window and rollback).
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    void RemoveByAttribution(const std::string& plugin_name);
    std::vector<std::string> OwnedBy(const std::string& plugin_name) const;

private:
    template <typename T>
    struct Owned {
        std::string plugin;
        std::shared_ptr<const T> value;
    };
    std::vector<Owned<ProviderEntryTranslator>> translators_;
    std::vector<Owned<SecretSource>> sources_;
    std::string attribution_;
};

}  // namespace minimal::config
