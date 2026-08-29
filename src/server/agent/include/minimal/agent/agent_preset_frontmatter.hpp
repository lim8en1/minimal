#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/config/frontmatter.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// Raw, unresolved decode of one agents/*.md file -- no cross-file lookups (no
// providers.json/permissions.json/settings.json access here). That composition
// is AgentPresetRegistry's job.
struct AgentPresetFrontmatter {
    std::string name;                              // required
    std::string description;                       // optional, default ""
    std::string provider;                          // required; "default" is a valid literal value
    std::string model;                             // optional override; empty = use provider entry's model_name
    std::vector<std::string> allowed_tools;         // default empty (fail closed; "*" must be explicit)
    std::vector<std::string> allowed_skills;        // default empty
    std::vector<std::string> allowed_mcp_servers;   // default empty
    tools::PermissionOverrideLayer permission_mode;  // read_only/file_write/execute/out_of_workdir + overrides -> per_tool_overrides
    std::unordered_map<std::string, tools::PermissionMode> mcp_permissions;  // server -> mode
    std::string compression_prompt;                 // filename under prompts/, "" = use default
    std::string body;                               // system prompt text, unresolved (no subagent.md prefix)
};

// Pure. Throws std::runtime_error on missing `name`/`provider`, or any
// permission-mode string outside allow|ask|deny (including an empty-string key
// in permission_mode.overrides -- see permissions_config.hpp for why that key
// is reserved).
AgentPresetFrontmatter ParseAgentPresetFrontmatter(const config::ParsedFrontmatter& parsed);

// Thin: reads the file, splits frontmatter/body, then parses. Throws if the
// file is missing.
AgentPresetFrontmatter LoadAgentPresetFrontmatter(const std::filesystem::path& path);

}  // namespace minimal::agent
