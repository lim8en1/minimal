#include "minimal/agent/subagent_manager.hpp"

#include "minimal/mcp/mcp_client_manager.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <utility>

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
                                  std::int64_t max_concurrent_subagents,
                                  std::string default_subagent_model, common::Logger& logger,
                                  mcp::McpClientManager* mcp_manager,
                                  std::shared_ptr<tools::PermissionAdvisor> advisor,
                                  bool advisor_inherit,
                                  std::chrono::milliseconds advisor_timeout,
                                  hooks::HookDispatcher* hooks)
    : sessions_(sessions),
      db_(db),
      presets_(presets),
      max_concurrent_subagents_(max_concurrent_subagents),
      default_subagent_model_(std::move(default_subagent_model)),
      logger_(logger),
      mcp_manager_(mcp_manager),
      advisor_(std::move(advisor)),
      advisor_inherit_(advisor_inherit),
      advisor_timeout_(advisor_timeout),
      hooks_(hooks) {}

SubagentOutcome SubagentManager::Spawn(const SubagentRequest& request,
                                        const tools::ToolContext& ctx) {
    const std::string parent_id = ctx.session_id;
    auto parent = sessions_.Find(parent_id);
    if (parent == nullptr) {
        return SubagentOutcome::Error("cannot spawn a subagent: the parent session is gone");
    }

    // Over-limit spawns are an error the model adapts to, not a queue it blocks
    // in -- a tool that hangs is a turn that never ends.
    //
    // Parallel dispatch made this check non-atomic: sibling spawns of one round
    // run concurrently, and a child is not yet countable in ChildCount when its
    // Spawn is still constructing it. The admission ticket below holds a place
    // from this check until CreateResolved has inserted the row, so every
    // concurrent checker sees either the ticket or the child, never neither.
    //
    // The ChildCount read is INSIDE limit_mu_, deliberately: read-then-check
    // splits admission into two unsynchronized halves, and a sibling whose read
    // landed before another's registry insert can then acquire the lock, find
    // the ticket already released, and admit a second child at the exact
    // boundary the ticket exists to protect. Lock order is limit_mu_ ->
    // registry mutex, one-directional: nothing holds the registry lock and then
    // calls into SubagentManager (the registry does not know this class
    // exists), so no reverse path can close a cycle.
    {
        std::lock_guard<std::mutex> lock(limit_mu_);
        const std::size_t live_children = sessions_.ChildCount(parent_id);
        const std::int64_t starting = starting_children_[parent_id];
        if (static_cast<std::int64_t>(live_children) + starting >= max_concurrent_subagents_) {
            return SubagentOutcome::Error("subagent limit reached (" +
                                          std::to_string(max_concurrent_subagents_) +
                                          " concurrent); wait for one to finish");
        }
        ++starting_children_[parent_id];
    }
    struct TicketGuard {
        SubagentManager& self;
        const std::string& parent_id;
        bool released = false;
        // Explicit release once the child is countable in ChildCount: the
        // ticket's job is done, so its place is given back. The conservative
        // window the comment at the release site describes -- a concurrent
        // checker seeing both the child and the still-held ticket, and
        // refusing at the exact limit boundary -- exists only between the
        // registry insert and this call. Idempotent, so the destructor can
        // simply call it: an error path that never reached the release site
        // gives the place back here.
        void Release() {
            if (released) return;
            {
                std::lock_guard<std::mutex> lock(self.limit_mu_);
                auto it = self.starting_children_.find(parent_id);
                if (it != self.starting_children_.end() && --it->second <= 0) {
                    self.starting_children_.erase(it);
                }
            }
            released = true;
        }
        ~TicketGuard() { Release(); }
    } ticket{*this, parent_id};

    // The parent's own preset is the default, addressed by the name it was
    // resolved with -- not the frontmatter display name, which need not match.
    const std::string preset_name =
        request.agent_preset.empty() ? parent->info().agent_preset : request.agent_preset;

    // The parent's spawn allowlist (agents/*.md allowed_subagents) is the gate
    // the spawn tool's per-preset description and enum advertise: a name the
    // schema never offers is also a name the manager refuses, so neither
    // surface can drift from the enforcement. nullopt = unrestricted ("*"
    // expanded at resolve); an empty list never reaches here because the loop
    // withholds the tool -- reaching this check at all means the model named
    // a preset explicitly, which the error below names.
    const auto& allowed_subagents = parent->preset().allowed_subagents;
    if (allowed_subagents.has_value() &&
        std::find(allowed_subagents->begin(), allowed_subagents->end(), preset_name) ==
            allowed_subagents->end()) {
        return SubagentOutcome::Error("agent preset '" + preset_name +
                                      "' is not in this agent's allowed_subagents");
    }

    // The child's model, by precedence: an explicit model on the spawn call
    // wins; then the parent session's /subagent-model choice; then the child
    // preset's own `model:` frontmatter; then settings.json's
    // default_subagent_model; and finally the parent's own model -- "same as
    // parent" -- which is the default this chain exists to express.
    std::optional<common::ModelRef> spawn_model = request.model;
    if (!spawn_model) spawn_model = parent->subagent_model_override();

    AgentPreset child_preset;
    try {
        if (spawn_model.has_value()) {
            // Rides straight into Resolve() (not just into params.model_override
            // below) so an unresolvable settings default cannot fail the spawn
            // ahead of the explicit choice -- the same rescue CreateResolved's
            // own model_override plumbing needs.
            child_preset = presets_.Resolve(preset_name, parent->info().project_root,
                                            parent->preset().allowed_skills, spawn_model);
        } else {
            // Nothing more specific: the child inherits its parent's model,
            // unless its preset declares one. Resolved WITH the fallback as the
            // override so a preset that declares no model -- and a missing or
            // unresolvable settings.default_model, which used to be this
            // spawn's failure mode -- cannot brick it. A DECLARED model is
            // re-resolved without the override so it keeps winning over the
            // inheritance default.
            std::optional<common::ModelRef> fallback;
            if (!default_subagent_model_.empty()) {
                if (auto ref = common::SplitModelRef(default_subagent_model_)) {
                    if (presets_.providers().Resolve(*ref) != nullptr) {
                        fallback = std::move(ref);
                    } else {
                        logger_.warn("settings default_subagent_model '{}' resolves to "
                                     "nothing in providers.json; behaving as unset",
                                     default_subagent_model_);
                    }
                } else {
                    logger_.warn("settings default_subagent_model '{}' is not a "
                                 "<provider>/<model> reference; behaving as unset",
                                 default_subagent_model_);
                }
            }
            if (!fallback) fallback = parent->preset().model_ref;
            child_preset = presets_.Resolve(preset_name, parent->info().project_root,
                                            parent->preset().allowed_skills, fallback);
            if (child_preset.model_declared) {
                child_preset = presets_.Resolve(preset_name, parent->info().project_root,
                                                parent->preset().allowed_skills, std::nullopt);
                spawn_model = std::nullopt;  // the preset's own choice stands
            } else {
                spawn_model = std::move(fallback);
            }
        }
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
    // Skills are already clamped INSIDE Resolve (against this parent's map) --
    // they must be, because a preload entry is consumed at resolve time and a
    // post-resolve clamp could not stop the prompt injection.
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
    // The spawn's model decision rides model_override: CreateResolved's
    // ResolveModel honors it for children too, so an unresolvable ref fails
    // the spawn with a named error before the child's first call. nullopt
    // means the child preset's own declared model stands (nothing above it
    // fired). The parent's /subagent-model choice itself is deliberately not
    // inherited as child session state -- it is the parent's choice for ITS
    // children, consumed here at spawn time.
    params.model_override = spawn_model;

    // Inheritance is OPT-IN for the advisor: the default child answers Ask
    // through its own wire checker -- the human -- even when the parent had an
    // advisor. With advisor_inherit set, the same SELECTION the top-level
    // producer records is recorded here; rpc::FinishSessionWiring composes it
    // against the CHILD's own channels when the registry funnel wires the
    // child. The ceilings above still bound what the child (advised or not)
    // may run.
    if (advisor_inherit_) {
        params.selected_advisor = advisor_;
        params.advisor_timeout = advisor_timeout_;
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
                               &logger, hooks = hooks_](const TurnResult& result) {
        completion->set_value(result);
        // core.subagent_completed: observe-only, fired from the child's
        // completion hook, which lands on EVERY exit path -- success, failure,
        // cancellation, teardown -- exactly the guarantee the blocking wait
        // already rests on. A hook watching subagent completions therefore
        // sees the same record a parent does.
        if (hooks != nullptr) {
            hooks->EmitAsync(hooks::kSubagentCompleted,
                             nlohmann::json{{"child_session_id", child_id},
                                            {"stop_reason", hooks::StopReasonName(result.stop_reason)},
                                            {"hit_max_turns", result.hit_max_turns},
                                            {"is_error", result.stop_reason !=
                                                                     llm::StopReason::EndTurn ||
                                                         result.final_text.empty()}});
        }
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
            // QueueInjectedMessage refuses for two reasons: the parent is
            // terminating, or its injection staging queue (64) is full. Either
            // way the result goes nowhere; saying "queued" sent the next reader
            // of this log chasing a message that was dropped, and naming only
            // "terminating" misread a queue-full drop the same way.
            logger.warn("subagent {} finished ({}); parent refused injection "
                        "(terminating or queue full), result dropped",
                        child_id, child_result.is_error ? "error" : "ok");
        }
    };

    std::shared_ptr<Session> child;
    try {
        // The preset's `steps` is a ceiling on the child's tool rounds: a spawn
        // request may lower it, never raise it (and an unbounded request under
        // a bounded preset inherits the bound). Computed before the move below.
        std::optional<int> turns = request.max_turns > 0
                                        ? std::optional<int>(request.max_turns)
                                        : std::nullopt;
        if (child_preset.steps.has_value() && *child_preset.steps > 0 &&
            (!turns.has_value() || *turns > *child_preset.steps)) {
            turns = *child_preset.steps;
        }
        child = sessions_.CreateResolved(std::move(params), std::move(child_preset),
                                         std::move(turns));
    } catch (const std::exception& e) {
        // Inserting first is what makes the database the id authority, but it
        // also means a construction failure would leave a message-less session
        // in the picker forever. Same rollback as SessionServiceImpl -- guarded
        // the same way: a delete failure here (locked/corrupt store) must not
        // replace the real construction error with the rollback's own, and
        // must not throw past this catch block into the tool-execution guard,
        // which would report "failed unexpectedly" instead of the actual
        // reason the spawn refused. The orphan stub row left behind on a
        // failed delete is inert (no messages, nothing else references it).
        try {
            db_.DeleteSession(row.session_id);
        } catch (const std::exception& rollback_error) {
            logger_.warn("session {}: spawn rollback delete failed: {}", row.session_id,
                         rollback_error.what());
        }
        return SubagentOutcome::Error(e.what());
    }
    // The child is countable in ChildCount from here; give the ticket's place
    // back. (Microscopically conservative: a concurrent checker between the
    // insert and this Release may see both the child and the ticket, and
    // refuse at the exact limit boundary -- the safe direction to err. Skipping
    // the decrement instead, as an earlier draft did, leaked the ticket on
    // every successful spawn: after max_concurrent_subagents successes -- even
    // sequential ones, all long finished -- every future spawn refused, the
    // count inflated permanently and the map growing monotonically.)
    ticket.Release();

    // Announce BEFORE the child produces anything: attaching to the child is a
    // client protocol obligation, and this event is what starts it. The child's
    // own events buffer in its queue until the client gets there.
    parent->EmitEvent(SessionEvent{SubagentStarted{child_id, request.task, blocking}});

    // core.subagent_started: observe-only, fired at the same moment as the
    // wire event above. The task text is the parent model's own formulation --
    // carried as payload data, deliberately not filterable.
    if (hooks_ != nullptr) {
        hooks_->EmitAsync(hooks::kSubagentStarted,
                          nlohmann::json{{"parent_session_id", parent_id},
                                         {"child_session_id", child_id},
                                         {"blocking", blocking},
                                         {"agent_preset", preset_name},
                                         {"task", request.task}});
    }

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
