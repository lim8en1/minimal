#pragma once

// Plugin extensions to providers.json parsing. Core parses only its own
// providers.json shape; a plugin that reads another tool's config (the
// opencode-compat plugin: opencode's provider blocks and its auth.json) does
// so here, so core never names that tool.
//
//   - A ProviderEntryTranslator rewrites one provider entry into core's shape
//     BEFORE core parses it. Translators run in registration order; each sees
//     the previous one's output. Throw to reject the entry (a config error,
//     naming `where`).
//   - A CredentialSource answers a provider's api key when the entry opted
//     into `"credentials": "auth-file"` and core's own <data dir>/auth.json has
//     none. Sources are asked in registration order; the first answer wins.
//
// Filled once, during plugin load (minimal_plugin_register_provider_config_
// extensions), before providers.json is read; read-only afterwards.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::config {

class ProviderEntryTranslator {
public:
    virtual ~ProviderEntryTranslator() = default;
    virtual void Translate(nlohmann::json& entry, const std::string& where) const = 0;
};

class CredentialSource {
public:
    virtual ~CredentialSource() = default;
    // Where the key would come from, for logs ("~/.local/share/foo/auth.json").
    virtual std::string Describe() const = 0;
    // The key for `provider_name`, or nullopt. Must not throw.
    virtual std::optional<std::string> Lookup(const std::string& provider_name) const = 0;
};

class ProviderConfigExtensions {
public:
    void AddTranslator(std::shared_ptr<const ProviderEntryTranslator> translator);
    void AddCredentialSource(std::shared_ptr<const CredentialSource> source);

    // Applies every translator to `entry`, in registration order.
    void Translate(nlohmann::json& entry, const std::string& where) const;
    // The first source's answer, with `origin` set to its Describe().
    std::optional<std::string> LookupCredential(const std::string& provider_name,
                                                std::string& origin) const;
    // Every source's Describe(), for the "no key found" warning.
    std::vector<std::string> DescribeCredentialSources() const;

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
    std::vector<Owned<CredentialSource>> sources_;
    std::string attribution_;
};

}  // namespace minimal::config
