#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"

namespace minimal::agent {

// Name + description only, for a picker to list -- see ListAvailable() below.
struct AgentPresetSummary {
    std::string name;
    std::string description;
};

// Loads providers.json/permissions.json/agents/*.md into fully resolved
// AgentPresets, plus the settings.json handed to it. Config is read once at
// startup -- there is no hot-reload (see ARCHITECTURE.md's Config section).
class AgentPresetRegistry {
public:
    // Loads providers.json and permissions.json from config_dir immediately
    // (throws if either is missing/malformed); settings.json arrives pre-parsed
    // via the constructor. agents/*.md files are
    // NOT scanned eagerly: each is loaded lazily on first Resolve() for that
    // name and cached, matching "computed once at load/switch time, never
    // re-resolved per tool call". prompts/subagent.md and prompts/compression.md
    // are read once here but treated as OPTIONAL at this phase (empty string if
    // absent) -- a future sanity_check is what makes their absence loud in a
    // real deployment.
    //
    // `skills` is shared with SkillTool rather than loaded again: one scan of
    // skills/, one set of load-status entries, and both consumers agree on what
    // exists. The listing rendered into each preset's system prompt comes from
    // it (allowed_skills ∩ loaded, see ResolveUncached).
    //
    // `settings` is passed in rather than parsed here: settings.json used to be
    // read three times at startup (main, the sanity check, and this class), and
    // main ran the loop on the sanity check's parse while this one resolved
    // default_model from its own -- the sanity check could validate content the
    // server was not running. One parse, in the sanity check, handed to both.
    AgentPresetRegistry(std::filesystem::path config_dir, const config::SkillRegistry& skills,
                        const config::SettingsConfig& settings);

    // Same as above, but takes an already-parsed PermissionsConfig instead of
    // loading permissions.json itself -- the production path (main.cpp),
    // once the startup sanity check has already loaded and validated it.
    // Delegated to by the 3-arg overload above, which still loads its own
    // copy for every other caller (tests construct this directly).
    AgentPresetRegistry(std::filesystem::path config_dir, const config::SkillRegistry& skills,
                        const config::SettingsConfig& settings,
                        const config::PermissionsConfig& permissions);

    // Throws if agents/<preset_name>.md doesn't exist, or its `provider`
    // doesn't resolve (a providers.json entry, or "default" with no
    // settings.json default_provider). The returned preset always has
    // is_subagent == false and an unprefixed system_prompt -- the prompts/subagent.md
    // prefixing and is_subagent=true are session-level facts layered on by a
    // future session/spawn-time caller, using SubagentPrefix() below.
    // BY VALUE, and the cache below is why. Returning a reference into a
    // container that a concurrent Resolve() may rehash hands the caller a
    // pointer that can die under it -- and every production caller copied the
    // result anyway, because a preset is per-session state a session then
    // mutates (--model, /agent).
    AgentPreset Resolve(const std::string& preset_name) const;

    // Lists every agents/*.md file's name + description WITHOUT resolving its
    // model/provider -- for a picker to show, not for the loop to run. A picker
    // listing one bad preset must not fail to show the rest, so a file that
    // fails to parse is logged and skipped rather than thrown. Sorted by name
    // for a deterministic picker order.
    std::vector<AgentPresetSummary> ListAvailable() const;

    const std::string& SubagentPrefix() const { return subagent_prefix_; }

    // The parsed providers.json this registry resolved presets against. Exposed
    // rather than re-loaded by callers who need to resolve a model the USER
    // named (--model, /model): two loads would be two sources of truth, and the
    // second could disagree after an edit.
    const config::ProvidersConfig& providers() const { return providers_; }

    // Discovery injection (track N): the hook built over the ModelCatalogRegistry
    // after plugin load. Must be called before the first Resolve()/model lookup
    // that could trigger a fetch -- main.cpp injects right after construction.
    void SetDiscoveryHook(config::DiscoveryHook hook) { providers_.SetDiscoveryHook(std::move(hook)); }

    // Evicts and refetches every discover-entry's catalog (see
    // ProvidersConfig::RefreshDiscovery). Const for the same reason: it touches
    // only mutex-guarded cache state, never the parsed config.
    std::vector<config::DiscoveryRefreshOutcome> RefreshDiscovery() const {
        return providers_.RefreshDiscovery();
    }

private:
    AgentPreset ResolveUncached(const std::string& preset_name) const;

    std::filesystem::path config_dir_;
    config::ProvidersConfig providers_;
    config::SettingsConfig settings_;
    config::PermissionsConfig permissions_;
    const config::SkillRegistry& skills_;
    std::string subagent_prefix_;
    std::string default_compression_prompt_;
    // GUARDED, because Resolve() populates it lazily and its callers are on
    // different threads with nothing between them: SessionRegistry::Create runs
    // on a gRPC handler thread (before it takes its own lock), and
    // SubagentManager::Spawn runs on a session worker during a tool call. Two
    // concurrent CreateSessions, or a CreateSession racing a spawn_subagent, is
    // a concurrent emplace into one unordered_map. The class comment's "config
    // is read once at startup, there is no hot-reload" is true of the FILES; the
    // cache is built at runtime, on demand, from whichever thread asks first.
    mutable std::mutex cache_mu_;
    mutable std::unordered_map<std::string, AgentPreset> cache_;
};

}  // namespace minimal::agent
