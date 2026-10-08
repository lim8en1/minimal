#pragma once

// What plugins see of preset resolution: the picker summary, the per-root
// environment and context a definition builder or prompt appendix reads, the
// registry those are registered in, and PresetCatalog -- the read side of the
// preset registry a command needs. Split out of agent_preset_registry.hpp,
// which holds the whole settings and permissions model and stays core's.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/llm/chat.hpp"

namespace minimal::config {
class ProjectTrust;
class ProvidersConfig;
}  // namespace minimal::config

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

// The preset registry's read side, as commands use it (/agent, /model,
// /compact's prompt, the trust commands). AgentPresetRegistry implements it.
class PresetCatalog {
public:
    virtual ~PresetCatalog() = default;

    // `preset_name` resolved for `project_root`, `parent` the spawning
    // session's preset on the subagent path, `model_override` applied.
    // Throws std::runtime_error naming what exists when it does not resolve.
    virtual AgentPreset Resolve(const std::string& preset_name,
                                const std::filesystem::path& project_root = {},
                                const AgentPreset* parent = nullptr,
                                const std::optional<config::ModelRef>& model_override =
                                    std::nullopt) const = 0;
    // prompts/<name>.md's body (a bare file name; throws otherwise or when
    // missing).
    virtual std::string ResolveCompressionPrompt(const std::string& name) const = 0;
    // settings.json default_model when it resolves, else the only model, else
    // nullopt.
    virtual std::optional<config::ModelRef> DefaultModel() const = 0;
    // Null when no trust store is wired (tests, tools).
    virtual config::ProjectTrust* project_trust() const = 0;
    virtual const config::ProvidersConfig& providers() const = 0;
};

}  // namespace minimal::agent
