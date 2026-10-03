#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/config/frontmatter.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// Per-skill access (opencode-compat). The ORDERING is the subagent clamp
// lattice: Preload is the WIDEST mode (the body ships in the system prompt,
// always on) and Deny the narrowest, so clamping a child takes the elementwise
// MAX -- a child can never widen into its parent's preloads, and a parent's
// deny beats every child request.
enum class SkillMode {
    Preload = 0,  // body appended to the system prompt; not callable via the tool
    Allow = 1,    // listed; the `skill` tool loads it outright
    Ask = 2,      // listed; the `skill` tool prompts (permission key `skill:<name>`)
    Deny = 3,     // absent from the listing; the `skill` tool refuses
};

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
// `disable`, `hidden`, `color`, deprecated `tools`). Unknown structural keys do
// NOT warn: any frontmatter key this parser does not recognize is carried in
// `extra_parameters` and passed to the provider as request parameters
// (opencode's "additional options" passthrough -- `reasoningEffort` and
// friends). The trade, accepted deliberately, is that a typo'd structural key
// becomes a harmless unknown parameter instead of a load error.
struct AgentPresetFrontmatter {
    std::string name;                              // required
    std::string description;                       // optional, default ""
    // ONE field, "<provider>/<model>". Was two (`provider` plus an optional
    // `model` override), which gave the same concept two syntaxes -- the preset
    // said `provider: local` + `model: qwen4` while /model and --model said
    // `local/qwen4`. Empty means "use settings.default_model".
    std::string model;
    std::vector<std::string> allowed_tools;         // default empty (fail closed; "*" must be explicit)
    // List form = every entry Allow (the native shape). Map form
    // (name: allow|ask|deny|preload) is the opencode-compat shape. "*" is legal
    // in both and means "every loaded skill", with the map form additionally
    // able to demote individual names below it ({"*": allow, "internal-*"-style
    // exact names: ask}).
    std::map<std::string, SkillMode> allowed_skills;
    // tool_sources: {<source>: [members]} -- the dynamic tool-source members
    // (tools/tool_source.hpp) this preset allows. The legacy
    // `allowed_mcp_servers: [...]` key lands here as tool_sources["mcp"].
    std::map<std::string, std::vector<std::string>> tool_sources;
    // Allowlist of agent preset keys THIS preset may spawn via spawn_subagent
    // (agents/<name>.md file stems -- the names a spawn request's agent_preset
    // accepts, not the frontmatter display `name:`). Absent = every spawnable
    // preset; an explicit list may contain "*" (= all); an explicit empty list
    // means no spawns at all -- the tool is withheld downstream, the same rule
    // as allowed_skills resolving empty. Enforced by SubagentManager::Spawn and
    // reflected in the spawn tool's per-preset description (the registry's
    // definition builder filters its listing with exactly this list).
    std::optional<std::vector<std::string>> allowed_subagents;
    tools::PermissionOverrideLayer permission_mode;  // read_only/file_write/execute/out_of_workdir/network + overrides -> per_tool_overrides
    // Keys inside `permission_mode:` the decoder did not recognise (a typo'd
    // `pattern:` or `file_writes:` used to vanish silently). Reported as
    // startup warnings by the sanity check.
    std::vector<std::string> unknown_permission_keys;
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
    // Unknown frontmatter keys, merged OVER the resolved model's parameters at
    // registry resolve time (a preset's extra beats the model's default).
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
// `known_tool_permission_keys`: the live ToolRegistry's permission-key
// vocabulary (AgentPresetRegistry::ToolPermissionKeys::all) -- what
// `permission:` block keys not in the fixed opencode-shape category set
// (read/glob/grep/list/edit/write/bash/webfetch/websearch/external_directory)
// are checked against before being accepted as a per-tool pattern-map alias.
// Structural, not a hardcoded literal list, so a renamed/aliased/third-party
// tool name works here exactly like the bundled ones. Required, not
// defaulted: an empty list is a real, distinguishable answer ("no tools
// known yet"), not "caller forgot".
AgentPresetFrontmatter ParseAgentPresetFrontmatter(
    const config::ParsedFrontmatter& parsed,
    const std::vector<std::string>& known_tool_permission_keys);

// Thin: reads the file, splits frontmatter/body, then parses. Throws if the
// file is missing.
AgentPresetFrontmatter LoadAgentPresetFrontmatter(
    const std::filesystem::path& path, const std::vector<std::string>& known_tool_permission_keys);

}  // namespace minimal::agent
