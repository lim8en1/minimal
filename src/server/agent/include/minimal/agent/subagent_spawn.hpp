#pragma once

// Subagent permission inheritance is clamped in three distinct places, and this
// header is where the pure half of it lives:
//
//   - run modes:      clamp_run_mode() -- already in tools/permission.hpp;
//   - policies:       the parent's policies are handed to the child's
//                     ToolExecutor as ceilings it folds in via most_restrictive()
//                     (see ToolExecutor);
//   - allowed lists:  intersect_allowed() below -- tools/skills/mcp servers the
//                     child may use are the intersection with the parent's.
//
// A child can never be more permissive than its parent (ARCHITECTURE.md,
// "Subagents").

#include <string>
#include <vector>

namespace minimal::agent {

// Intersects a child preset's allowed list with its parent's. A list containing
// "*" means "everything", so it is identity for the other side; otherwise the
// result keeps exactly the names present in both, in the child's order and
// without duplicates. An empty parent therefore yields an empty list whatever
// the child asks for -- including a child asking for "*".
std::vector<std::string> intersect_allowed(const std::vector<std::string>& parent,
                                           const std::vector<std::string>& child);

// What the model handed the spawn tool. Validated by the tool before the
// manager ever sees one.
struct SubagentRequest {
    std::string task;
    std::string agent_preset;  // empty = the parent's own preset
    bool blocking = true;
    int max_turns = 0;  // 0 = unbounded; a subagent's life is one RunTurn
};

// What the spawn produced, for the tool to turn into a ToolResultBlock.
struct SubagentOutcome {
    enum class Kind {
        kCompleted,  // blocking child finished; final_text is the result
        kStarted,    // non-blocking child accepted; it reports via injection
        kError,      // refused or failed; error names the reason
    };
    Kind kind = Kind::kError;
    std::string child_session_id;
    std::string final_text;
    std::string error;

    static SubagentOutcome Error(std::string message) {
        SubagentOutcome out;
        out.kind = Kind::kError;
        out.error = std::move(message);
        return out;
    }
};

}  // namespace minimal::agent
