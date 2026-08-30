#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
    std::string system_prompt;  // subagent.md prefix (if any) already concatenated
    llm::ProviderConfig provider;  // model_name already reflects preset-override-wins-else-entry
    std::vector<std::string> allowed_tools;  // {"*"} means all registered
    std::vector<std::string> allowed_skills;
    std::vector<std::string> allowed_mcp_servers;
    nlohmann::json parameters = nlohmann::json::object();  // from the resolved providers.json entry, not preset frontmatter

    tools::PermissionPolicy permission_mode;
    tools::McpPermissionPolicy mcp_permissions;
    tools::PermissionRunMode run_mode = tools::PermissionRunMode::Normal;
    std::string compression_prompt;  // loaded text, not a filename

    // Set for any session with a non-null parent_session_id. Drives the
    // unconditional exclusion of the subagent-spawn tool -- see
    // AgentLoop::BuildChatRequest.
    bool is_subagent = false;
};

// The one tool name the loop treats specially. Excluding it is a hard rule keyed
// on is_subagent, not a preset field, so a misconfigured preset cannot reopen
// recursion.
inline constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

// The core skill accessor. Offered only when the preset allows at least one
// skill -- see AgentLoop::BuildChatRequest. A preset with no skills gets no
// tool that can only return errors.
inline constexpr const char* kSkillToolName = "skill";

}  // namespace minimal::agent
