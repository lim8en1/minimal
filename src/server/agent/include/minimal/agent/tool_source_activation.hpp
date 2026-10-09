#pragma once

// Brings up the dynamic tool-source members a preset names (MCP servers are the
// first). Pure of Session: it takes the preset's map and the registry and
// answers which groups came up and why the rest did not.

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "minimal/tools/tool_source.hpp"

namespace minimal::agent {

struct ToolSourceFailure {
    std::string source_name;
    std::string member;
    std::string message;
};

struct ToolSourceActivation {
    // Registry groups of every member whose source is loaded, in preset
    // order -- including the members that failed: their groups stay active
    // (empty) so the tools appear when the member comes up later.
    std::vector<std::string> groups;
    std::vector<ToolSourceFailure> failures;  // members not up now, in preset order
};

// A fresh reappraisal every call, not a sticky blocklist. Members activate in
// PARALLEL under one shared `budget`, so one hung server costs at most the
// budget, not its full connect + tools/list timeouts. A member past the
// deadline is reported as a failure but its group stays active; its detached
// activation thread owns everything it touches, so nothing dangles when it
// finishes later -- and its tools then appear in the session.
// `sources` may be null: every named member then fails as "not available".
ToolSourceActivation ActivateToolSources(
    const std::map<std::string, std::vector<std::string>>& tool_sources,
    const tools::ToolSourceRegistry* sources, std::chrono::steady_clock::duration budget);

}  // namespace minimal::agent
