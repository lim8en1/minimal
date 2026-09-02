#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/config/providers_config.hpp"
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
    // RESOLVED, not the raw frontmatter list: `*` is already expanded and names
    // that are unknown or failed to load are already dropped, so this is exactly
    // the set the system-prompt listing advertises. Keeping it raw let the
    // listing and the tool-exposure gate be computed from different conditions
    // in different files, which is how they drifted -- `allowed_skills: [typo]`
    // offered the model a `skill` tool with nothing listed and no call that
    // could succeed. Empty means the tool is withheld entirely.
    std::vector<std::string> allowed_skills;
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
