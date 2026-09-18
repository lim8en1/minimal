#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// The resolved preset the loop programs against -- config precedence chains are
// applied once at load/switch time, never re-resolved per tool call. Populated
// by AgentPresetRegistry::Resolve(); see agent_preset_registry.hpp for how each
// field is computed from providers.json/settings.json/permissions.json/agents/*.md.
struct AgentPreset {
    std::string name;
    std::string description;
    std::string system_prompt;  // prompts/subagent.md prefix (if any) already concatenated
    llm::ProviderConfig provider;  // model_name already reflects preset-override-wins-else-entry
    // The providers.json `provider` field -- the kind string that selects a
    // factory from ProviderFactoryRegistry. Carried here because ProviderConfig
    // holds only connection details (base_url/api_key/model_name), so without it
    // a resolved preset cannot actually construct the provider it describes.
    std::string provider_kind;
    std::vector<std::string> allowed_tools;  // {"*"} means all registered
    // RESOLVED spawn allowlist, from the frontmatter's allowed_subagents:
    // nullopt = every spawnable preset (the field was absent, or named "*");
    // a present list -- possibly EMPTY -- is the exact set of preset keys a
    // spawn_subagent call may name. Enforced by SubagentManager::Spawn; the
    // empty case also withholds the tool entirely (AgentLoop::BuildChatRequest),
    // the same fail-closed rule as allowed_skills resolving empty.
    std::optional<std::vector<std::string>> allowed_subagents;
    // RESOLVED, not the raw frontmatter map: `*` is already expanded and names
    // that are unknown or failed to load are already dropped, so this is exactly
    // the set the system-prompt listing advertises. Keeping it raw let the
    // listing and the tool-exposure gate be computed from different conditions
    // in different files, which is how they drifted -- `allowed_skills: [typo]`
    // offered the model a `skill` tool with nothing listed and no call that
    // could succeed. Holds ONLY Preload/Allow/Ask entries: explicit denies
    // resolve out, because absence IS deny -- which is what makes the subagent
    // clamp's "parent without the entry" rule uniform. Empty means the tool is
    // withheld entirely. (Preload entries are consumed at resolve time -- their
    // bodies ship in the system prompt -- and carry no tool-side access.)
    std::map<std::string, SkillMode> allowed_skills;
    std::vector<std::string> allowed_mcp_servers;
    nlohmann::json parameters = nlohmann::json::object();  // from the resolved providers.json entry, not preset frontmatter
    // Also from the providers.json entry. Applies to every request the session
    // makes, summarization included -- a cap the conversation honours and the
    // compactor ignores is how a summary gets truncated.
    std::optional<int> max_tokens;
    // Which <provider>/<model> this resolved from. Carried because the
    // connection plus the model id do not identify it: two providers can serve
    // the same model at the same url with different limits.
    config::ModelRef model_ref;
    // True when model_ref came from the preset's own frontmatter `model:`
    // declaration rather than a settings.json default. A spawn consults this
    // to keep "the child inherits its parent's model" from clobbering an
    // explicit per-agent declaration.
    bool model_declared = false;
    // The window the compaction trigger's window-relative strategies (percent /
    // buffer) resolve against, and the status bar's used/max (track E). It rides
    // on the PRESET rather than on LoopConfig because /agent and /model can both
    // switch models mid-session, and the trigger has to track the window
    // actually in use. nullopt means the model declared none, and those
    // strategies never fire -- see compaction_trigger.hpp.
    std::optional<std::int64_t> context_window_tokens;

    tools::PermissionPolicy permission_mode;
    tools::McpPermissionPolicy mcp_permissions;
    tools::PermissionRunMode run_mode = tools::PermissionRunMode::Normal;
    std::string compression_prompt;  // loaded text, not a filename
    // The resolved `compaction_strategy:` frontmatter, verbatim: a NAME, not a
    // strategy object -- the preset cache is built before the strategy
    // registry is fully loaded, and the loop resolves the name at each
    // compaction attempt against the registry Session borrows (see
    // AgentLoop::Compact). Empty = the settings-level strategy stands.
    std::string compaction_strategy;

    // Preset-scoped tool definitions, produced by the builders registered on
    // AgentPresetRegistry (SetDefinitionBuilder) and run ONCE at resolve time
    // -- their results ride the frozen preset, so a definition is computed
    // lazily per (preset, project root) and static for every session running
    // it. AgentLoop::BuildSystemAndTools replaces a returned definition whose
    // name matches a key, before the subagent/skill strips. Constant per
    // session, so the request prefix the provider's prompt cache keys on
    // never shifts; core tools not covered by a builder keep their static
    // spec() definition, and MCP definitions are never overridden.
    std::map<std::string, llm::ToolDefinition> tool_definition_overrides;

    // Per-tool/per-plugin configuration from the preset's `tool_config`
    // frontmatter map, VERBATIM ({namespace: {...}}). Delivered per-invocation
    // to each tool as ToolContext::tool_config (its own config_namespace()
    // slice), so a mid-session /agent re-resolve takes effect at the next tool
    // call. Core interprets nothing; the claiming plugin owns its slice.
    nlohmann::json tool_config = nlohmann::json::object();

    // --- opencode-compat surface (see agent_preset_frontmatter.hpp) ------------
    // Visibility-only: spawn_subagent accepts any registered preset whatever
    // the mode; mode decides whether the agent picker lists the preset.
    AgentMode mode = AgentMode::All;
    bool hidden = false;    // excluded from the picker listing even with mode All
    std::string color;      // "#rrggbb" for the picker; other values render as the default
    // Ceiling on tool rounds when this preset runs AS A SUBAGENT (spawn
    // requests may lower it, never raise it). nullopt = unbounded. Applies to
    // nothing else: a top-level session is the operator's to interrupt.
    std::optional<int> steps;

    // Set for any session with a non-null parent_session_id. Drives the
    // unconditional exclusion of the subagent-spawn tool -- see
    // AgentLoop::BuildChatRequest.
    bool is_subagent = false;
};

// Stamps a resolved model onto a preset, replacing whatever it resolved to.
//
// One function because there are two callers with the same obligation: session
// creation applying --model, and /model applying a mid-session switch. Missing a
// field in one of them -- carrying the old max_tokens onto a new model, say --
// is the kind of bug that only shows up as a truncated response much later.
void ApplyModel(AgentPreset& preset, const config::ResolvedModel& model);

// The one tool name the loop treats specially. Excluding it is a hard rule keyed
// on is_subagent, not a preset field, so a misconfigured preset cannot reopen
// recursion.
inline constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

// The core skill accessor. Offered only when the preset allows at least one
// skill -- see AgentLoop::BuildChatRequest. A preset with no skills gets no
// tool that can only return errors.
inline constexpr const char* kSkillToolName = "skill";

}  // namespace minimal::agent
