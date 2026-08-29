#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// The resolved preset the loop programs against -- config precedence chains are
// applied once at load/switch time, never re-resolved per tool call.
//
// Minimal for now: skills, MCP servers, and compression_prompt land alongside
// their own subsystems rather than being declared here before anything reads
// them.
struct AgentPreset {
    std::string name;
    std::string system_prompt;  // subagent.md prefix (if any) already concatenated
    std::vector<std::string> allowed_tools;  // {"*"} means all registered
    std::string model;
    nlohmann::json parameters = nlohmann::json::object();

    tools::PermissionPolicy permission_mode;
    tools::PermissionRunMode run_mode = tools::PermissionRunMode::Normal;

    // Set for any session with a non-null parent_session_id. Drives the
    // unconditional exclusion of the subagent-spawn tool -- see
    // AgentLoop::BuildChatRequest.
    bool is_subagent = false;
};

// The one tool name the loop treats specially. Excluding it is a hard rule keyed
// on is_subagent, not a preset field, so a misconfigured preset cannot reopen
// recursion.
inline constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

}  // namespace minimal::agent
