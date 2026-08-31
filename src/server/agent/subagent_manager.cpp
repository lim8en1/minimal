#include "minimal/agent/subagent_manager.hpp"

#include "minimal/mcp/mcp_client_manager.hpp"

#include <chrono>
#include <future>
#include <utility>

#include "minimal/rpc/wire_permission_checker.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::agent {
namespace {

// What the parent sees of a finished child, whatever the mode. Cancelled and
// failed turns are errors with a reason; a natural stop with no text is an
// error too, because a silent empty result reads like success.
struct ChildResult {
    bool is_error;
    std::string text;
};

ChildResult ResultOf(const TurnResult& result) {
    if (result.stop_reason == llm::StopReason::Cancelled) {
        return {true, "subagent was terminated before it finished"};
    }
    if (result.final_text.empty()) {
        if (result.hit_max_turns) {
            return {true, "subagent reached its max_turns limit without producing a result"};
        }
        if (result.stop_reason == llm::StopReason::Unknown) {
            return {true, "subagent failed to produce a result"};
        }
        return {true, "subagent finished without producing any text"};
    }
    return {false, result.final_text};
}

}  // namespace

SubagentManager::SubagentManager(SessionRegistry& sessions, session::SessionDatabase& db,
                                  const AgentPresetRegistry& presets,
                                  std::int64_t max_concurrent_subagents, common::Logger& logger,
                                  mcp::McpClientManager* mcp_manager,
                                  std::shared_ptr<tools::PermissionAdvisor> advisor,
                                  bool advisor_inherit,
                                  std::chrono::milliseconds advisor_timeout)
    : sessions_(sessions),
      db_(db),
      presets_(presets),
      max_concurrent_subagents_(max_concurrent_subagents),
      logger_(logger),
      mcp_manager_(mcp_manager),
      advisor_(std::move(advisor)),
      advisor_inherit_(advisor_inherit),
      advisor_timeout_(advisor_timeout) {}

SubagentOutcome SubagentManager::Spawn(const SubagentRequest& request,
                                        const tools::ToolContext& ctx) {
    const std::string parent_id = ctx.session_id;
    auto parent = sessions_.Find(parent_id);
    if (parent == nullptr) {
        return SubagentOutcome::Error("cannot spawn a subagent: the parent session is gone");
    }

    // Over-limit spawns are an error the model adapts to, not a queue it blocks
    // in -- a tool that hangs is a turn that never ends.
    const std::size_t live_children = sessions_.ChildCount(parent_id);
    if (static_cast<std::int64_t>(live_children) >= max_concurrent_subagents_) {
        return SubagentOutcome::Error("subagent limit reached (" +
                                      std::to_string(max_concurrent_subagents_) +
                                      " concurrent); wait for one to finish");
    }

    // The parent's own preset is the default, addressed by the name it was
    // resolved with -- not the frontmatter display name, which need not match.
    const std::string preset_name =
        request.agent_preset.empty() ? parent->info().agent_preset : request.agent_preset;

    AgentPreset child_preset;
    try {
        child_preset = presets_.Resolve(preset_name);
    } catch (const std::exception& e) {
        return SubagentOutcome::Error(e.what());
    }

    // --- Inheritance: a child is never more permissive than its parent -------
    //
    // The run mode is clamped here and carried as the child's override; the
    // policies travel as ceilings the child's ToolExecutor folds in at resolve
    // time; held grants are copied; allowed lists are intersected. The spawn
    // tool itself cannot reappear whatever the lists say -- BuildChatRequest
    // excludes it from any session with is_subagent set.
    child_preset.is_subagent = true;
    const std::string& prefix = presets_.SubagentPrefix();
    if (!prefix.empty()) {
        child_preset.system_prompt = prefix + "\n" + child_preset.system_prompt;
    }
    child_preset.allowed_tools =
        intersect_allowed(parent->preset().allowed_tools, child_preset.allowed_tools);
    child_preset.allowed_skills =
        intersect_allowed(parent->preset().allowed_skills, child_preset.allowed_skills);
    child_preset.allowed_mcp_servers =
        intersect_allowed(parent->preset().allowed_mcp_servers, child_preset.allowed_mcp_servers);

    // A subagent whose preset requires a dead server DOES NOT START: an error
    // ToolResultBlock the parent model adapts to, before any child row exists.
    // Checked here, not in the child's construction, so the failure names the
    // SERVER and lands as the spawn tool's result.
    if (mcp_manager_ != nullptr) {
        for (const auto& server : child_preset.allowed_mcp_servers) {
            std::string error;
            if (!mcp_manager_->EnsureAlive(server, error)) {
                return SubagentOutcome::Error("subagent preset '" + preset_name +
                                              "' requires MCP server '" + server +
                                              "' which is unavailable: " + error);
            }
        }
    }

    // The DATABASE mints the id, so the row exists before the Session does --
    // the same ordering SessionServiceImpl::CreateSession owns, with the same
    // rollback obligation below. The parent's row exists by construction, so
    // the parent_session_id foreign key holds.
    session::SessionSummary row;
    try {
        row = db_.CreateSession(/*name=*/"", parent_id, parent->info().project_root.string(),
                                preset_name);
    } catch (const std::exception& e) {
        return SubagentOutcome::Error(std::string("could not record the subagent session: ") +
                                      e.what());
    }

    SessionParams params;
    params.session_id = row.session_id;
    params.parent_session_id = parent_id;
    params.agent_preset = preset_name;
    params.project_root = parent->info().project_root;
    params.run_mode_override = tools::clamp_run_mode(parent->run_mode(), child_preset.run_mode);
    params.seed_grants = parent->granted_keys();
    params.policy_ceiling = parent->policy();
    params.mcp_policy_ceiling = parent->mcp_policy();
    params.mcp_manager = mcp_manager_;

    // Inheritance is OPT-IN for the advisor: the default child answers Ask
    // through its own wire checker -- the human -- even when the parent had an
    // advisor. With advisor_inherit set, the same composition the top-level
    // producer uses is repeated here, against the CHILD's channels; the
    // ceilings above still bound what the child (advised or not) may run.
    if (advisor_inherit_ && advisor_) {
        params.channels = MakeSessionChannels();
        auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
        params.permission_checker = std::make_unique<tools::AdvisedPermissionChecker>(
            advisor_,
            std::make_unique<rpc::WirePermissionChecker>(params.session_id, params.channels->queue,
                                                         params.channels->pending),
            cancel_source, advisor_timeout_);
        params.advisor_cancel_source = std::move(cancel_source);
    }

    // The completion signal. Blocking waits on it here; non-blocking uses it on
    // the child's worker to queue the parent's injection. Fired on every exit
    // path of the child's single turn.
    auto completion = std::make_shared<std::promise<TurnResult>>();
    const std::string tool_use_id = ctx.tool_use_id;
    const std::string child_id = row.session_id;
    const bool blocking = request.blocking;
    std::shared_ptr<Session> parent_ref = parent;
    common::Logger& logger = logger_;
    params.on_turn_finished = [completion, blocking, parent_ref, child_id, tool_use_id,
                               &logger](const TurnResult& result) {
        completion->set_value(result);
        if (blocking) return;  // the waiter translates the result itself

        // Non-blocking: the result arrives at the parent's next turn boundary,
        // as a tool-result-only user message keyed to the spawn call -- the
        // shape the session-naming rule already knows not to title.
        const ChildResult child_result = ResultOf(result);
        llm::ToolResultBlock block;
        block.tool_use_id = tool_use_id;
        block.is_error = child_result.is_error;
        block.content.push_back(llm::TextBlock{child_result.text});
        if (parent_ref->QueueInjectedMessage(llm::Message{llm::Role::User, {std::move(block)}})) {
            logger.info("subagent {} finished ({}); result queued for its parent", child_id,
                        child_result.is_error ? "error" : "ok");
        } else {
            // The parent is already terminating; the result goes nowhere. Saying
            // "queued" here sent the next reader of this log chasing a message
            // that was dropped.
            logger.warn("subagent {} finished ({}); parent already terminating, result dropped",
                        child_id, child_result.is_error ? "error" : "ok");
        }
    };

    std::shared_ptr<Session> child;
    try {
        child = sessions_.CreateResolved(std::move(params), std::move(child_preset),
                                         request.max_turns > 0
                                             ? std::optional<int>(request.max_turns)
                                             : std::nullopt);
    } catch (const std::exception& e) {
        // Inserting first is what makes the database the id authority, but it
        // also means a construction failure would leave a message-less session
        // in the picker forever. Same rollback as SessionServiceImpl.
        db_.DeleteSession(row.session_id);
        return SubagentOutcome::Error(e.what());
    }

    // Announce BEFORE the child produces anything: attaching to the child is a
    // client protocol obligation, and this event is what starts it. The child's
    // own events buffer in its queue until the client gets there.
    v1::ServerEvent started_event;
    started_event.set_session_id(parent_id);
    auto* started = started_event.mutable_subagent_started();
    started->set_child_session_id(child_id);
    started->set_task(request.task);
    started->set_blocking(blocking);
    parent->queue().Push(std::move(started_event));

    child->SubmitTurn(request.task);
    logger_.info("session {}: spawned {} subagent {} (preset '{}')", parent_id,
                 blocking ? "blocking" : "non-blocking", child_id, preset_name);

    if (!blocking) {
        SubagentOutcome outcome;
        outcome.kind = SubagentOutcome::Kind::kStarted;
        outcome.child_session_id = child_id;
        return outcome;
    }

    // Blocking: wait on the parent's worker -- the thread that is allowed to
    // block -- polling the parent's cancellation so a CancelTurn or a closing
    // client propagates into the child instead of being waited out. Terminating
    // the child is what unblocks this wait: its completion hook fires during
    // the teardown join, which is also why the loop can simply break and read
    // the future.
    auto future = completion->get_future();
    while (future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
        if (ctx.is_cancelled && ctx.is_cancelled()) {
            sessions_.Terminate(child_id);
            break;
        }
    }
    const ChildResult child_result = ResultOf(future.get());
    logger_.info("session {}: blocking subagent {} done ({})", parent_id, child_id,
                 child_result.is_error ? "error" : "ok");
    if (child_result.is_error) {
        return SubagentOutcome::Error(child_result.text);
    }
    SubagentOutcome outcome;
    outcome.kind = SubagentOutcome::Kind::kCompleted;
    outcome.child_session_id = child_id;
    outcome.final_text = std::move(child_result.text);
    return outcome;
}

}  // namespace minimal::agent
