#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"

namespace minimal::agent {

// Loads providers.json/settings.json/permissions.json/agents/*.md into fully
// resolved AgentPresets. Config is read once at startup -- there is no
// hot-reload (see ARCHITECTURE.md's Config section).
class AgentPresetRegistry {
public:
    // Loads providers.json, settings.json, permissions.json from config_dir
    // immediately (throws if any is missing/malformed). agents/*.md files are
    // NOT scanned eagerly: each is loaded lazily on first Resolve() for that
    // name and cached, matching "computed once at load/switch time, never
    // re-resolved per tool call". agents/subagent.md and prompts/compression.md
    // are read once here but treated as OPTIONAL at this phase (empty string if
    // absent) -- a future sanity_check is what makes their absence loud in a
    // real deployment.
    explicit AgentPresetRegistry(std::filesystem::path config_dir);

    // Throws if agents/<preset_name>.md doesn't exist, or its `provider`
    // doesn't resolve (a providers.json entry, or "default" with no
    // settings.json default_provider). The returned preset always has
    // is_subagent == false and an unprefixed system_prompt -- subagent.md
    // prefixing and is_subagent=true are session-level facts layered on by a
    // future session/spawn-time caller, using SubagentPrefix() below.
    const AgentPreset& Resolve(const std::string& preset_name) const;

    const std::string& SubagentPrefix() const { return subagent_prefix_; }

private:
    AgentPreset ResolveUncached(const std::string& preset_name) const;

    std::filesystem::path config_dir_;
    config::ProvidersConfig providers_;
    config::SettingsConfig settings_;
    config::PermissionsConfig permissions_;
    std::string subagent_prefix_;
    std::string default_compression_prompt_;
    mutable std::unordered_map<std::string, AgentPreset> cache_;
};

}  // namespace minimal::agent
