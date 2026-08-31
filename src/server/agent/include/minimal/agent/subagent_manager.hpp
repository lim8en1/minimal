#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>

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
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::mcp {
class McpClientManager;
}  // namespace minimal::mcp

namespace minimal::agent {

class SubagentManager {
public:
    // `mcp_manager` may be null (no mcps.json, MCP-free tests): the child then
    // simply has no live MCP servers. When present it gates the spawn: a child
    // preset naming a dead server FAILS THE SPAWN -- an error ToolResultBlock
    // the parent model adapts to, never a child that starts crippled.
    SubagentManager(SessionRegistry& sessions, session::SessionDatabase& db,
                    const AgentPresetRegistry& presets, std::int64_t max_concurrent_subagents,
                    common::Logger& logger, mcp::McpClientManager* mcp_manager = nullptr,
                    std::shared_ptr<tools::PermissionAdvisor> advisor = nullptr,
                    bool advisor_inherit = false,
                    std::chrono::milliseconds advisor_timeout = tools::AdvisedPermissionChecker::kDefaultTimeout,
                    hooks::HookDispatcher* hooks = nullptr);

    // Runs where blocking is allowed -- the parent's worker, or a tool-pool
    // thread when the spawn was dispatched in a parallel round (still a tool
    // execution context, still only ever blocked on bounded things: the child's
    // turn, which cancellation terminates). A blocking request returns only
    // after the child's single turn has finished (or the parent was cancelled,
    // which terminates the child and returns an error instead of hanging).
    SubagentOutcome Spawn(const SubagentRequest& request, const tools::ToolContext& ctx);

private:
    SessionRegistry& sessions_;
    session::SessionDatabase& db_;
    const AgentPresetRegistry& presets_;
    std::int64_t max_concurrent_subagents_;
    common::Logger& logger_;
    mcp::McpClientManager* mcp_manager_ = nullptr;
    // The configured advisor, resolved once at startup; copied into a child's
    // params ONLY when advisor_inherit_ is set -- a child defaults to no
    // advisor (docs/EXTENSIBILITY.md §3.4). The parent's ceilings still bound
    // the child: they are enforced by the child's ToolExecutor before its
    // checker (advised or not) is ever consulted.
    std::shared_ptr<tools::PermissionAdvisor> advisor_;
    bool advisor_inherit_ = false;
    std::chrono::milliseconds advisor_timeout_;

    // Parallel-dispatch accounting (ARCHITECTURE.md "Tool dispatch"). Spawn can
    // now run concurrently with sibling spawns of the same round, which breaks
    // the old check-then-register story: the limit reads SessionRegistry::
    // ChildCount, but a child only becomes countable when CreateResolved
    // inserts it -- two simultaneous spawns at limit 1 would both pass the
    // check before either child existed. An admission ticket closes the gap:
    // check-and-increment under limit_mu_, held from the check until the
    // registry's count picks the child up (or the spawn fails, where the guard
    // below decrements instead). The count is per parent; entries are erased
    // when they hit zero.
    std::mutex limit_mu_;
    std::unordered_map<std::string, std::int64_t> starting_children_;
    // The process-wide hook dispatcher (track O). Borrowed; null in tests.
    // Emissions from here: core.subagent_started (before the child produces
    // anything) and core.subagent_completed (from the child's completion hook,
    // which fires on every exit path). The CHILD's own emissions -- its tools'
    // pre_tool_use, its user_prompt_submit -- are wired through the registry
    // funnel setting SessionParams::hooks, not here.
    hooks::HookDispatcher* hooks_ = nullptr;
};

}  // namespace minimal::agent
