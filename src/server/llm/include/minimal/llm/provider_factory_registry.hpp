#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/llm/provider.hpp"

namespace minimal::llm {

// Maps a providers.json `provider` string to a factory. This is why there is no
// ProviderKind enum: an enum could not name a kind supplied by a plugin core has
// never heard of, and providers.json already carries the value as a string.
//
// Distinct from the (future) ProviderInstanceCache, which the two were once
// conflated under one name: this holds kind-string -> factory, that holds
// constructed instances shared across concurrent sessions.
class ProviderFactoryRegistry {
public:
    using Factory = std::function<std::unique_ptr<LlmProvider>(const ProviderConfig&)>;

    // Returns false if `kind` was already registered. FIRST REGISTRATION WINS,
    // matching ToolRegistry and CommandRegistry, and the loser is recorded in
    // collisions() rather than merely rejected: the return value alone was
    // ignored by the only caller, so a second plugin registering `openai` was
    // dropped with no diagnostic anywhere and which backend you actually got
    // depended on plugin load order. That is the exact failure the name
    // collision rule exists to prevent, and provider kinds were the one
    // registration namespace exempt from it.
    bool Register(std::string kind, Factory factory);

    // --- Attribution, for collision reporting -------------------------------
    //
    // The same scoped window ToolRegistry and CommandRegistry use, for the same
    // reason: Register() has no idea who is calling, and a collision has to be
    // able to name BOTH plugins. Not thread-safe and does not need to be --
    // plugin loading is single-threaded and finishes before any session exists.
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Collision {
        std::string kind;
        std::string first_plugin;   // the registration that holds the slot
        std::string second_plugin;  // the one that was dropped
    };
    const std::vector<Collision>& collisions() const { return collisions_; }

    bool Has(std::string_view kind) const;

    // Returns nullptr for an unregistered kind. A providers.json entry naming a
    // kind no plugin supplied is a startup sanity-check failure, not a crash.
    std::unique_ptr<LlmProvider> Create(std::string_view kind, const ProviderConfig& config) const;

    // Sorted, for deterministic diagnostics and /providers output.
    std::vector<std::string> kinds() const;

private:
    std::unordered_map<std::string, Factory> factories_;
    std::unordered_map<std::string, std::string> registrant_;  // kind -> plugin that holds it
    std::string attribution_;
    std::vector<Collision> collisions_;
};

}  // namespace minimal::llm
