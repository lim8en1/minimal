#pragma once

// Everything a spawn_subagent call does besides parsing its input. A subagent
// is the SAME Session entity as a top-level conversation -- one field
// (parent_session_id) plus restrictions derived from it -- so this class is
// mostly about deriving those restrictions and wiring the two sessions
// together:
//
//   - permission inheritance: policies handed down as ceilings, the run mode
//     clamped, held grants copied, allowed lists intersected;
//   - the SubagentStartedEvent on the parent's queue, which starts the
//     client's attach obligation;
//   - blocking mode: the parent's worker waits here for the child's single
//     turn to finish, observing the parent's cancellation all the while;
//   - non-blocking mode: the child runs on its own worker and its completion
//     is queued for injection at the parent's next turn boundary.
//
// See ARCHITECTURE.md "Subagents".

#include <cstdint>
#include <memory>
#include <string>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/subagent_spawn.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::agent {

class SubagentManager {
public:
    SubagentManager(SessionRegistry& sessions, session::SessionDatabase& db,
                    const AgentPresetRegistry& presets, std::int64_t max_concurrent_subagents,
                    common::Logger& logger,
                    std::shared_ptr<tools::PermissionAdvisor> advisor = nullptr,
                    bool advisor_inherit = false);

    // Runs on the PARENT's worker thread -- it is a tool execution, and that is
    // the thread permitted to block. A blocking request returns only after the
    // child's single turn has finished (or the parent was cancelled, which
    // terminates the child and returns an error instead of hanging).
    SubagentOutcome Spawn(const SubagentRequest& request, const tools::ToolContext& ctx);

private:
    SessionRegistry& sessions_;
    session::SessionDatabase& db_;
    const AgentPresetRegistry& presets_;
    std::int64_t max_concurrent_subagents_;
    common::Logger& logger_;
    // The configured advisor, resolved once at startup; copied into a child's
    // params ONLY when advisor_inherit_ is set -- a child defaults to no
    // advisor (docs/EXTENSIBILITY.md §3.4). The parent's ceilings still bound
    // the child: they are enforced by the child's ToolExecutor before its
    // checker (advised or not) is ever consulted.
    std::shared_ptr<tools::PermissionAdvisor> advisor_;
    bool advisor_inherit_ = false;
};

}  // namespace minimal::agent
