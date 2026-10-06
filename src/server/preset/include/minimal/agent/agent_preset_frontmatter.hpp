#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/config/frontmatter.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// How a preset may be surfaced (opencode-compat `mode`). VISIBILITY-ONLY by
// design: `spawn_subagent` accepts any registered preset, and mode never gates
// spawnability -- a primary preset named by a command with `subtask: true` runs
// as a child exactly like a dedicated subagent. What mode decides is whether
// the preset appears in the top-level agent picker (Subagent: no).
enum class AgentMode {
    All,        // default: picker-visible and spawnable
    Primary,    // picker-visible (spawnable, as always)
    Subagent,   // spawnable but hidden from the agent picker
};

// Raw, unresolved decode of one agents/*.md file -- no cross-file lookups (no
// providers.json/permissions.json/settings.json access here). That composition
// is AgentPresetRegistry's job.
//
// opencode-compat aliases are accepted beside the native fields and folded
// into them at parse time (`permission` -> permission_mode, `mode`, `steps`,
// `disable`, `hidden`, `color`). Strict: an unknown key is an error. Provider
// request parameters go under `parameters:`; opencode's top-level
// `temperature`, `top_p` and `reasoningEffort` are accepted beside it.
struct AgentPresetFrontmatter {
    std::string name;                              // required
    std::string description;                       // optional, default ""
    // ONE field, "<provider>/<model>". Was two (`provider` plus an optional
    // `model` override), which gave the same concept two syntaxes -- the preset
    // said `provider: local` + `model: qwen4` while /model and --model said
    // `local/qwen4`. Empty means "use settings.default_model".
    std::string model;
    std::vector<std::string> allowed_tools;         // default empty (fail closed; "*" must be explicit)
    // plugins: [<plugin name>] -- every tool those plugins register, added to
    // allowed_tools at resolve (AgentPresetRegistry). Visibility only: the
    // permission modes still decide each call.
    std::vector<std::string> plugins;
    // tool_sources: {<source>: [members]} -- the dynamic tool-source members
    // (tools/tool_source.hpp) this preset allows.
    std::map<std::string, std::vector<std::string>> tool_sources;
    // Allowlist of agent preset keys THIS preset may spawn via spawn_subagent
    // (agents/<name>.md file stems -- the names a spawn request's agent_preset
    // accepts, not the frontmatter display `name:`). Absent = every spawnable
    // preset; an explicit list may contain "*" (= all); an explicit empty list
    // means no spawns at all -- the tool is withheld downstream, the same rule
    // as a definition builder withholding a tool. Enforced by SubagentManager::Spawn and
    // reflected in the spawn tool's per-preset description (the registry's
    // definition builder filters its listing with exactly this list).
    std::optional<std::vector<std::string>> allowed_subagents;
    tools::PermissionOverrideLayer permission_mode;  // read_only/file_write/execute/out_of_workdir/network + overrides -> per_tool_overrides
    // plugin_permissions: {<model>: {...}} -- each plugin permission model's
    // preset slice, verbatim (YamlToJson lowering). Core never reads inside a
    // slice; the model's BuildLayer does. Empty object when absent.
    nlohmann::json plugin_permissions = nlohmann::json::object();
    std::string compression_prompt;                 // filename under prompts/, "" = use default
    // Optional override of memory.compaction_strategy for THIS agent: the name
    // of a registered CompactionStrategy kind ("summarize" = free-text head,
    // "structured" = the forced record_summary tool-call format). Resolved
    // against the strategy registry at each compaction attempt -- see
    // AgentLoop::Compact. Empty = the settings-level strategy stands; a name
    // nothing registered warns once per session and degrades the same way
    // (a typo must not brick compaction).
    std::string compaction_strategy;
    std::string body;                               // system prompt text, unresolved (no prompts/subagent.md prefix)

    // --- opencode-compat surface ----------------------------------------------
    AgentMode mode = AgentMode::All;
    bool disable = false;   // preset refuses to load (Resolve throws; listing skips)
    bool hidden = false;    // excluded from the agent picker listing, still spawnable
    std::string color;      // "#rrggbb" for the picker; anything else is ignored client-side
    // Ceiling on the subagent's tool rounds when THIS preset is spawned as a
    // child (a spawn request may lower it, never raise it). nullopt = unbounded.
    std::optional<int> steps;
    // The preset's provider parameters (`parameters:` plus the allowlisted
    // top-level keys), merged OVER the resolved model's parameters at
    // registry resolve time (a preset's value beats the model's default).
    nlohmann::json extra_parameters = nlohmann::json::object();
    // Per-tool/per-plugin configuration, keyed by the namespace a tool's
    // config_namespace() claims: `tool_config: {python: {venv: ".venv"}}`.
    // Carried VERBATIM -- core never interprets a slice's contents; the plugin
    // that claims the namespace owns its schema. A structural (non-passthrough)
    // key, so it can never leak into extra_parameters and reach the provider
    // as a request parameter.
    nlohmann::json tool_config = nlohmann::json::object();
};

// Pure. Throws std::runtime_error on a missing `name`, a `model` that is not
// <provider>/<model> (validated here so a typo fails at load rather than at the
// first turn), or any
// permission-mode string outside allow|ask|deny (including an empty-string key
// in permission_mode.overrides -- see permissions_config.hpp for why that key
// is reserved).
//
// The live tool vocabulary a preset's `permission:` block resolves against,
// taken from ToolRegistry once tools are registered (never a literal list):
//   permission_keys  -- every live tool's permission key;
//   config_aliases   -- Tool::config_aliases(): foreign spelling -> key;
//   no_pattern_rules -- keys whose tool takes no pattern rules.
// Implicit from a key list for callers (tests) that only need the keys.
struct ToolVocabulary {
    ToolVocabulary() = default;
    ToolVocabulary(std::vector<std::string> keys) : permission_keys(std::move(keys)) {}  // NOLINT
    std::vector<std::string> permission_keys;
    std::map<std::string, std::string> config_aliases;
    std::vector<std::string> no_pattern_rules;
};

AgentPresetFrontmatter ParseAgentPresetFrontmatter(const config::ParsedFrontmatter& parsed,
                                                   const ToolVocabulary& tools);

// Thin: reads the file, splits frontmatter/body, then parses. Throws if the
// file is missing.
AgentPresetFrontmatter LoadAgentPresetFrontmatter(const std::filesystem::path& path,
                                                  const ToolVocabulary& tools);

}  // namespace minimal::agent
