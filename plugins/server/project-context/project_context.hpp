#pragma once

// Project context files -- AGENTS.md, CLAUDE.md -- read from a session's
// project_root and appended to its system prompt. One file wins: the first
// candidate found in the search order (settings_config.hpp, ProjectContextSettings)
// is loaded whole and the search stops.
//
// Like SkillRegistry, the loader never throws and treats absence as the normal
// state -- most project directories have neither file. The one failure worth
// distinguishing is "a candidate exists but could not be read", which is
// surfaced through the optional error out-param rather than silently folded
// into "not found".

#include <filesystem>
#include <optional>
#include <string>

#include <cstdint>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/context_contributor.hpp"

namespace minimal::project_context {

// Project context files (AGENTS.md, CLAUDE.md): read once per session from the
// session's project_root -- at creation, resume, and subagent spawn -- and
// appended to the system prompt. This is a SESSION property, not a preset one:
// a mid-session /agent re-resolves the preset and must not drop the project's
// instructions (see AgentLoop::BuildChatRequest).
//
// `files` is tried in order WITHIN each directory. With search_ancestors the
// search also walks upward from project_root, nearest directory first -- so a
// CLAUDE.md sitting IN the project beats an AGENTS.md three levels up, because
// closer instructions are more specific. Walking stops at the filesystem root,
// which makes a ~/AGENTS.md a global fallback; that is a feature, not an
// accident to fence off. max_bytes == 0 means unlimited. A symlinked file
// that resolves outside the directory it was found in is ignored.
struct ProjectContextSettings {
    std::vector<std::string> files = {"AGENTS.md", "CLAUDE.md"};
    std::int64_t max_bytes = 262144;
    bool search_ancestors = false;
};

// The context contributor: this project's instructions section, at priority
// 0 (ahead of every other context section), sized by settings.max_bytes.
std::shared_ptr<agent::ContextContributor> MakeProjectContextContributor(
    ProjectContextSettings settings);

// The plugin's plugins.json `config`: {"files": [...], "max_bytes": N,
// "search_ancestors": bool}; every key optional. Throws on a bad value.
ProjectContextSettings ParseProjectContextSettings(const nlohmann::json& config);


struct ProjectContext {
    // Verbatim file text, truncated to ProjectContextSettings::max_bytes when
    // that is set and the file is larger.
    std::string body;
    std::filesystem::path source_path;
    // The winning file NAME ("AGENTS.md") -- what the rendered section names.
    std::string file_name;
    bool truncated = false;
    // Valid when truncated: the file's size before truncation, for the marker.
    std::uintmax_t total_bytes = 0;
};

// nullopt means nothing was loaded: disabled, no candidate file present, or
// every candidate empty. A found-but-unreadable file sets *error (when given)
// and returns nullopt, so the caller can log a reason instead of leaving the
// operator to wonder why instructions they wrote were ignored.
std::optional<ProjectContext> LoadProjectContext(const std::filesystem::path& project_root,
                                                 const ProjectContextSettings& settings,
                                                 std::string* error = nullptr);

// The system-prompt section for a loaded context: a header naming the file,
// then the body. The caller supplies any blank-line separation -- the section
// itself starts at "##". Empty for an empty body (which the loader already
// refuses, so this only guards direct calls).
std::string RenderProjectContextSection(const ProjectContext& context);

}  // namespace minimal::project_context
