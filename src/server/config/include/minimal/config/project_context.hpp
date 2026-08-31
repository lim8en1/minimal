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

#include "minimal/config/settings_config.hpp"

namespace minimal::config {

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

}  // namespace minimal::config
