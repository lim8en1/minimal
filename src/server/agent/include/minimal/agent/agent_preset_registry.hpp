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

// Per-root environment the definition builders read: material that varies by
// project root but not by preset, memoized once per canonical root and shared
// by every builder run under it. Plain data, so a builder stays a pure
// function of (preset, env) and the tools layer never enters the picture.
struct PresetToolEnv {
    // Spawnable presets for the root: every parseable, non-disabled
    // agents/*.md, project shadowing preserved, `hidden` and `mode: subagent`
    // INCLUDED -- spawnability is independent of picker visibility. Name is
    // the preset KEY (file stem), what a spawn request's agent_preset accepts.
    std::vector<AgentPresetSummary> available_agents;
    // The merged skill view for the same root (global ∪ project overlay,
    // sorted), for builders that render names + descriptions.
    std::vector<config::SkillMeta> skills;
};

// Everything a definition builder may read. `preset_key` is carried separately
// from the preset because it is NOT the same string: presets are addressed by
// their file stem everywhere a spawn or /agent names one, while
// AgentPreset::name is the frontmatter display value, which need not match
// (see ListAvailable's stem-keying note).
struct PresetToolContext {
    const AgentPreset& preset;
    const std::string& preset_key;
    const PresetToolEnv& env;
};

// Returns the definition the named tool advertises TO THIS PRESET: the
// builder's one shot at a preset-scoped description/schema. Runs once per
// (preset, root) inside ResolveUncached -- lazily, with the rest of the
// resolve -- and its result rides AgentPreset::tool_definition_overrides.
using PresetToolDefinitionBuilder =
    std::function<llm::ToolDefinition(const PresetToolContext&)>;

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

    // A cheap peek at whether agents/<preset_name>.md declares its own
    // `model:` frontmatter field -- just the path search + frontmatter parse,
    // none of Resolve()'s provider/skill resolution. For a caller that needs
    // model_declared to DECIDE which model_override to pass a subsequent
    // Resolve() (e.g. a subagent spawn falling back to the parent's model
    // only when the preset has none of its own): peeking here first means
    // that one Resolve() call is the only full resolve paid for, instead of
    // resolving once speculatively and again once model_declared is known.
    // Throws the same way Resolve() does for an unknown preset name.
    bool PresetDeclaresModel(const std::string& preset_name,
                             const std::filesystem::path& project_root = {}) const;

    // Lists every agents/*.md file's name + description WITHOUT resolving its
    // model/provider -- for a picker to show, not for the loop to run. A picker
    // listing one bad preset must not fail to show the rest, so a file that
    // fails to parse is logged and skipped rather than thrown. Sorted by name
    // for a deterministic picker order. A non-empty project_root merges the
    // project-local agents/ directories into the listing (project shadows
    // global on name collision).
    std::vector<AgentPresetSummary> ListAvailable(
        const std::filesystem::path& project_root = {}) const;

    // Resolves a compression prompt BY NAME -- the per-invocation override
    // /compact <name> uses. Same rules a preset frontmatter `compression_prompt:`
    // value follows: a bare file name (no '/' or '\\', never "." or "..") naming
    // prompts/<name>.md under the config dir; the frontmatter body is returned.
    // Throws with the reason on an invalid name or a missing file; the caller
    // decides whether that is a command error or something louder.
    std::string ResolveCompressionPrompt(const std::string& name) const;

    // Registers the builder that produces `tool_name`'s definition per preset.
    // Startup-only wiring (main.cpp, beside the tool's own registration);
    // every builder runs inside ResolveUncached for each resolved preset, and
    // a preset resolved before its builder was registered would carry no
    // override -- so all builders MUST be set before the first Resolve().
    // Overwriting a name replaces the builder; there is deliberately no
    // unregister (config is read once at startup).
    void SetDefinitionBuilder(std::string tool_name, PresetToolDefinitionBuilder builder);

    // The `skill` tool's preset-scoped definition: the static base schema with
    // this preset's resolved (clamped, expanded, preload-consumed) skill
    // listing appended to the description -- the migration of what used to be
    // rendered into the system prompt, onto the generic builder mechanism.
    static PresetToolDefinitionBuilder MakeSkillDefinitionBuilder();

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

    // Project-local-shadows-global path search shared by ResolveUncached and
    // PresetDeclaresModel. Throws "unknown agent preset" if neither exists.
    // Does NOT log the shadow warning -- ResolveUncached does that itself
    // (it already needs from_project for the check), so a peek stays silent
    // and the warning still fires exactly once per real resolve.
    std::filesystem::path FindPresetPath(const std::string& preset_name,
                                         const std::filesystem::path& project_root,
                                         bool& from_project) const;

    // The shared loader behind a preset frontmatter `compression_prompt:` value
    // and ResolveCompressionPrompt(): validates the bare-filename rule (the name
    // becomes a path into prompts/, and presets and clients are untrusted
    // content -- '/' or '\\' would climb out and read an arbitrary *.md into
    // the summarization prompt), then reads prompts/<name>.md and returns the
    // frontmatter body. Throws on violation or missing file. `error_context`
    // prefixes the message -- the preset name where one exists, so a resolve
    // failure for preset X is attributable to X.
    std::string LoadCompressionPromptFile(const std::string& name,
                                          const std::string& error_context) const;

    // The one merged agents/ scan both listings run, parameterized by
    // visibility: the picker listing hides `hidden` and `mode: subagent`
    // presets; the spawnable listing includes them, because spawn addresses
    // presets by key whatever the picker shows. Disabled and malformed files
    // are skipped exactly the same by both.
    std::vector<AgentPresetSummary> ListPresets(const std::filesystem::path& project_root,
                                                bool picker_visibility) const;

    // The shared per-root builder inputs (memoized; see env_cache_ below).
    const PresetToolEnv& EnvFor(const std::filesystem::path& project_root) const;

    std::filesystem::path config_dir_;
    config::ProvidersConfig providers_;
    config::SettingsConfig settings_;
    config::PermissionsConfig permissions_;
    const config::SkillRegistry& skills_;
    std::string subagent_prefix_;
    std::string default_compression_prompt_;
    // Startup-only in practice (main.cpp wires builders beside the tool
    // registrations), but guarded because ResolveUncached reads it from
    // session-creation threads and a stray late SetDefinitionBuilder must not
    // race a concurrent resolve.
    std::map<std::string, PresetToolDefinitionBuilder> definition_builders_;
    // The builders' per-root inputs, computed lazily on the first resolve
    // that needs them and memoized per canonical root: N presets of one root
    // scan agents/ and skills/ once, and a deployment with no builders never
    // scans at all. Same lifetime assumption as the preset cache -- files do
    // not change at runtime. GUARDED (populated from ResolveUncached, whose
    // callers are on different threads); std::map so a returned reference
    // stays valid across later inserts.
    mutable std::map<std::filesystem::path, PresetToolEnv> env_cache_;
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
