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
    std::vector<std::string> allowed_mcp_servers;   // default empty
    tools::PermissionOverrideLayer permission_mode;  // read_only/file_write/execute/out_of_workdir/network + overrides -> per_tool_overrides
    std::unordered_map<std::string, tools::PermissionMode> mcp_permissions;  // server -> mode
    std::string compression_prompt;                 // filename under prompts/, "" = use default
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
};

// Pure. Throws std::runtime_error on a missing `name`, a `model` that is not
// <provider>/<model> (validated here so a typo fails at load rather than at the
// first turn), or any
// permission-mode string outside allow|ask|deny (including an empty-string key
// in permission_mode.overrides -- see permissions_config.hpp for why that key
// is reserved).
AgentPresetFrontmatter ParseAgentPresetFrontmatter(const config::ParsedFrontmatter& parsed);

// Thin: reads the file, splits frontmatter/body, then parses. Throws if the
// file is missing.
AgentPresetFrontmatter LoadAgentPresetFrontmatter(const std::filesystem::path& path);

}  // namespace minimal::agent
