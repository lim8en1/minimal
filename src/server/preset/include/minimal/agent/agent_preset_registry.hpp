#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/project_trust.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"

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
};

// Everything a definition builder may read. `preset_key` is carried separately
// from the preset because it is NOT the same string: presets are addressed by
// their file stem everywhere a spawn or /agent names one, while
// AgentPreset::name is the frontmatter display value, which need not match
// (see ListAvailable's stem-keying note).
// `project_root` is the root the preset resolves for (empty: no project
// scope). `parent` is the spawning session's preset on the subagent path, null
// otherwise: an extension that grants something per preset clamps a child to
// what its parent has (the subagent ceiling, applied at resolve time).
struct PresetToolContext {
    const AgentPreset& preset;
    const std::string& preset_key;
    const PresetToolEnv& env;
    const std::filesystem::path& project_root;
    const AgentPreset* parent = nullptr;
};

// Returns the definition the named tool advertises TO THIS PRESET: the
// builder's one shot at a preset-scoped description/schema. Runs once per
// (preset, root) inside ResolveUncached -- lazily, with the rest of the
// resolve -- and its result rides AgentPreset::tool_definition_overrides.
// A definition with an EMPTY name means "this preset does not get the tool"
// (the skill tool with no skill allowed): the request leaves it out.
using PresetToolDefinitionBuilder =
    std::function<llm::ToolDefinition(const PresetToolContext&)>;

// Text appended to a preset's system prompt at resolve time (the skills
// plugin's preloaded skill bodies). Empty: nothing appended.
using PresetPromptAppendix = std::function<std::string(const PresetToolContext&)>;

// What plugins add to preset resolution: per-tool definition builders and
// system-prompt appendices. Filled during plugin load
// (minimal_plugin_register_preset_extensions), read-only afterwards.
class PresetExtensionRegistry {
public:
    void AddDefinitionBuilder(std::string tool_name, PresetToolDefinitionBuilder builder);
    void AddPromptAppendix(PresetPromptAppendix appendix);

    struct Builder {
        std::string plugin;
        std::string tool_name;
        PresetToolDefinitionBuilder build;
    };
    struct Appendix {
        std::string plugin;
        PresetPromptAppendix render;
    };
    const std::vector<Builder>& builders() const { return builders_; }
    const std::vector<Appendix>& appendices() const { return appendices_; }

    // Plugin-loader bookkeeping (registration window and rollback).
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    void RemoveByAttribution(const std::string& plugin_name);
    std::vector<std::string> OwnedBy(const std::string& plugin_name) const;

private:
    std::vector<Builder> builders_;
    std::vector<Appendix> appendices_;
    std::string attribution_;
};

// The live ToolRegistry's structural tool-name vocabulary, resolved ONCE
// (main.cpp, after ToolRegistry::ApplyAliases) and handed down rather than a
// ToolRegistry& itself -- presets, like permission resolution, must resolve
// without consulting ToolRegistry directly (see permission_resolution.hpp).
// Both lists come from ToolRegistry and are always supplied together, hence
// one struct instead of two loose constructor parameters. Consulted by
// preset-frontmatter parsing (agent_preset_frontmatter.cpp's
// AliasKeyIsToolName) and by BuildPermissionPolicy's dotenv defaults.
struct ToolPermissionKeys {
    std::vector<std::string> all;              // ToolRegistry::PermissionKeys()
    std::vector<std::string> dotenv_protected;  // ToolRegistry::DotenvProtectedPermissionKeys()
    std::map<std::string, std::string> config_aliases;  // ToolRegistry::ConfigAliasPermissionKeys()
    std::vector<std::string> no_pattern_rules;  // ToolRegistry::NoPatternRulePermissionKeys()
    // ToolRegistry::PermissionKeysByPlugin(): what a preset's `plugins:` entry expands to.
    std::map<std::string, std::vector<std::string>> by_plugin;

    ToolVocabulary vocabulary() const {
        ToolVocabulary out(all);
        out.config_aliases = config_aliases;
        out.no_pattern_rules = no_pattern_rules;
        return out;
    }
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
    // `settings` is passed in rather than parsed here, so the server validates
    // and runs on the same parse (config::LoadConfigBundle in production).
    // `tool_permission_keys` defaults to {} so a caller with no ToolRegistry
    // handy (most tests) keeps compiling -- dotenv defaults and preset
    // pattern-map aliases simply see no known tools in that case.
    //
    // `variant_params` defaults to nullptr for the same reason: this
    // constructor's own LoadProvidersConfig call is where a static
    // "variants": [...] entry derives its wire parameters (see
    // VariantParametersFor), and it runs DURING construction, so the registry
    // must arrive as a constructor parameter.
    AgentPresetRegistry(std::filesystem::path config_dir,
                        const config::SettingsConfig& settings,
                        ToolPermissionKeys tool_permission_keys = {},
                        const llm::VariantParamsRegistry* variant_params = nullptr);

    // Same as above, but takes an already-parsed PermissionsConfig instead of
    // loading permissions.json itself. Still loads and owns its own
    // providers.json -- the self-contained form tests construct directly.
    AgentPresetRegistry(std::filesystem::path config_dir,
                        const config::SettingsConfig& settings,
                        const config::PermissionsConfig& permissions,
                        ToolPermissionKeys tool_permission_keys = {},
                        const llm::VariantParamsRegistry* variant_params = nullptr);

    // The production form (main.cpp): every config file already loaded once
    // (config::ConfigBundle), and the model catalog BORROWED from its owner,
    // which also injects the catalog's discovery/metadata/variant hooks. The
    // catalog must outlive this registry.
    AgentPresetRegistry(std::filesystem::path config_dir,
                        const config::SettingsConfig& settings,
                        const config::PermissionsConfig& permissions,
                        const config::ProvidersConfig& providers,
                        ToolPermissionKeys tool_permission_keys = {});

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
    // and what every global-only caller passes -- means no project scope. Extensions (builders,
    // prompt appendices) see the same root.
    //
    // `parent` (the spawn path) is the spawning session's preset, handed to
    // every extension so it can clamp the child to what the parent has BEFORE
    // anything is baked into the prompt -- a post-resolve clamp could not undo
    // an injected body. Uncached when given: the clamp input varies per parent,
    // and spawn is rare enough to pay a resolve every time.
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
    // Uncached for the same reason `parent` is: the override varies per
    // caller and session creation is not a hot path.
    AgentPreset Resolve(
        const std::string& preset_name, const std::filesystem::path& project_root = {},
        const AgentPreset* parent = nullptr,
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
    // Project-local agents are only discovered for roots `trust` allows; for the
    // others a session resolves exactly as if it had no project root (global and
    // bundled presets only). Null (the default) applies no gate. Startup only:
    // call before the registry is shared; `trust` must outlive it. No cache
    // invalidation is needed on a trust change -- the root is reduced to "no
    // project" BEFORE every cache lookup, so a revoked root never reads an entry
    // built while it was trusted.
    void SetProjectTrust(config::ProjectTrust* trust) { trust_ = trust; }
    // The same object, for the /trust-directory commands (which grant and
    // revoke). Null when no gate is configured.
    config::ProjectTrust* project_trust() const { return trust_; }

    void SetDefinitionBuilder(std::string tool_name, PresetToolDefinitionBuilder builder);

    // The plugins' builders and prompt appendices, consulted on every
    // resolve beside SetDefinitionBuilder's. Startup-only, before the first
    // Resolve(); must outlive this registry. Null: none.
    void SetExtensions(const PresetExtensionRegistry* extensions) { extensions_ = extensions; }

    // The plugin permission models every resolved preset binds its slices to
    // (AgentPreset::permission_models). Same startup-only rule as the
    // builders above: set after plugin load, before the first Resolve(). The
    // registry must outlive this one. Null (the default) binds nothing.
    void SetPermissionModels(const tools::PermissionModelRegistry* models) {
        permission_models_ = models;
    }

    const std::string& SubagentPrefix() const { return subagent_prefix_; }

    // The model catalog this registry resolves presets against. Exposed
    // rather than re-loaded by callers who need to resolve a model the USER
    // named (--model, /model): two loads would be two sources of truth.
    const config::ProvidersConfig& providers() const { return providers_; }
    // The model a primary session runs on when none was chosen (`/model
    // default` returns to it): settings.json default_model when it resolves,
    // else the ONLY model configured (static or already discovered; variants
    // of one model count once), else nullopt -- several candidates and no
    // usable setting means the user picks.
    std::optional<config::ModelRef> DefaultModel() const;
    // The same rule for the agent preset of a new session: settings.json
    // default_agent_preset when it names a resolvable preset, else the only
    // picker-visible preset for `project_root`, else nullopt.
    std::optional<std::string> DefaultAgentPreset(
        const std::filesystem::path& project_root = {}) const;
    // "a, b, c" -- what a "pick one" error lists.
    std::string DescribeAgentPresets(const std::filesystem::path& project_root = {}) const;

private:
    // Exactly one of `owned` / `borrowed` is set.
    AgentPresetRegistry(std::filesystem::path config_dir,
                        const config::SettingsConfig& settings,
                        const config::PermissionsConfig& permissions,
                        std::unique_ptr<config::ProvidersConfig> owned,
                        const config::ProvidersConfig* borrowed,
                        ToolPermissionKeys tool_permission_keys);

    AgentPreset ResolveUncached(const std::string& preset_name,
                                const std::filesystem::path& project_root,
                                const AgentPreset* parent,
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
    // Set only by the self-contained constructors; providers_ refers to it
    // then, and to the borrowed catalog otherwise.
    std::unique_ptr<config::ProvidersConfig> owned_providers_;
    const config::ProvidersConfig& providers_;
    config::SettingsConfig settings_;
    config::PermissionsConfig permissions_;
    ToolPermissionKeys tool_permission_keys_;
    const tools::PermissionModelRegistry* permission_models_ = nullptr;
    const PresetExtensionRegistry* extensions_ = nullptr;
    std::string subagent_prefix_;
    std::string default_compression_prompt_;
    // Startup-only in practice (main.cpp wires builders beside the tool
    // registrations), but guarded because ResolveUncached reads it from
    // session-creation threads and a stray late SetDefinitionBuilder must not
    // race a concurrent resolve.
    std::map<std::string, PresetToolDefinitionBuilder> definition_builders_;
    // The builders' per-root inputs, computed lazily on the first resolve
    // that needs them and memoized per canonical root: N presets of one root
    // scan agents/ once, and a deployment with no builders never
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
    // `project_root`, or empty when its project-local content may not load.
    std::filesystem::path EffectiveRoot(const std::filesystem::path& project_root) const {
        return trust_ == nullptr || trust_->Allows(project_root) ? project_root
                                                                 : std::filesystem::path{};
    }
    config::ProjectTrust* trust_ = nullptr;

    mutable std::mutex cache_mu_;
    mutable std::map<std::pair<std::string, std::filesystem::path>, AgentPreset> cache_;
};

}  // namespace minimal::agent
