#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <mutex>
#include <string>

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
    std::string color;  // "#rrggbb" from frontmatter; empty = theme default
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
    //
    // project_root anchors project-local discovery: <dir>/agents/<name>.md is
    // searched along the ProjectDirs walk (nearest git worktree root included)
    // BEFORE config_dir/agents/<name>.md, so a project can override a global
    // preset by name (the shadowing is logged once per resolve, and the cache
    // makes that once per (name, root)). An empty project_root -- the default,
    // and what every global-only caller passes -- means no project scope. The
    // project skill listing and allowed_skills resolution consult the same
    // root, so a preset resolved for one project advertises exactly the skills
    // SkillTool will serve in it.
    //
    // parent_skills (the spawn path) clamps the resolved skill modes BEFORE the
    // system prompt is baked: an elementwise most-restrictive fold against the
    // parent's map (Preload widest, Deny narrowest; a skill the parent lacks is
    // deny). The clamp has to live here because a preload entry is CONSUMED at
    // resolve time -- its body ships in the prompt -- so a post-resolve clamp
    // could not stop the injection. Uncached when given: the clamp input varies
    // per parent, and spawn is rare enough to pay a resolve every time.
    //
    // model_override, when given, is what the CALLER already decided the
    // session's model is (--model, /model, a fork's pinned choice, or a
    // spawn's inherited override) -- it outranks both the preset's own
    // `model:` and settings.json's default_model, and its resolution failure
    // (unknown model) is reported in terms of the override the user actually
    // named, not a placeholder default_model they never touched. This is the
    // difference between "the user's model choice failed to resolve" and "the
    // session cannot start because of a settings.json value nobody asked
    // about": without this, an unresolvable default_model throws here before
    // the caller's own ResolveModel-style override application ever runs.
    // Uncached for the same reason parent_skills is: the override varies per
    // caller and session creation is not a hot path.
    AgentPreset Resolve(
        const std::string& preset_name, const std::filesystem::path& project_root = {},
        const std::optional<std::map<std::string, SkillMode>>& parent_skills = std::nullopt,
        const std::optional<config::ModelRef>& model_override = std::nullopt) const;

    // Lists every agents/*.md file's name + description WITHOUT resolving its
    // model/provider -- for a picker to show, not for the loop to run. A picker
    // listing one bad preset must not fail to show the rest, so a file that
    // fails to parse is logged and skipped rather than thrown. Sorted by name
    // for a deterministic picker order. A non-empty project_root merges the
    // project-local agents/ directories into the listing (project shadows
    // global on name collision).
    std::vector<AgentPresetSummary> ListAvailable(
        const std::filesystem::path& project_root = {}) const;

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
    AgentPreset ResolveUncached(
        const std::string& preset_name, const std::filesystem::path& project_root,
        const std::optional<std::map<std::string, SkillMode>>& parent_skills,
        const std::optional<config::ModelRef>& model_override) const;

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
    // a concurrent emplace into one map. The class comment's "config is read
    // once at startup, there is no hot-reload" is true of the FILES; the cache
    // is built at runtime, on demand, from whichever thread asks first.
    //
    // Keyed by (preset name, canonical project root): project-local discovery
    // makes a preset a function of the session's root, and two sessions with
    // different roots resolve the same name independently. std::map over
    // unordered_map because the pair key needs a hash otherwise, and preset
    // counts are small.
    mutable std::mutex cache_mu_;
    mutable std::map<std::pair<std::string, std::filesystem::path>, AgentPreset> cache_;
};

}  // namespace minimal::agent
