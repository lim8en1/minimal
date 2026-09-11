#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::tools {

// The output of the gating phase, and everything Execute needs. Owns the
// resolved tool's shared_ptr (the registry hands out copies precisely so an
// in-flight invocation survives a concurrent MCP replacement) and, when the
// gate refused the call, the error result that answers it. A denial is a VALUE
// rather than an error state so Execute stays total: it returns the block it
// was given and touches nothing else.
struct PreparedInvocation {
    std::string tool_use_id;
    std::string tool_name;
    nlohmann::json input;
    std::shared_ptr<Tool> tool;  // null iff denial is set
    std::optional<llm::ToolResultBlock> denial;
    // The operator denied this call AND asked to end the turn ("deny and
    // stop"). Only ever set alongside `denial`; the agent loop honors it after
    // the round's gating -- no further call of the round runs, and no provider
    // call follows.
    bool stop_turn = false;

    // The confinement verdict Prepare's out-of-workdir fold used, carried so
    // Execute can re-check it (TOCTOU: the two phases are separated by the rest
    // of the round's gating, including Ask prompts that can sit for minutes).
    // checked=false when the fold never ran -- tool not path-bearing, no path
    // extractable, or the call already denied.
    bool path_confinement_checked = false;
    bool was_inside_project_root = false;
};

// The single call site the agent loop uses for tool invocation. Permission
// gating lives entirely here: Tool::execute() implementations never reference
// PermissionChecker, so the safety model cannot vary per tool.
//
// THREAD CONTRACT (parallel dispatch -- see ARCHITECTURE.md "Tool dispatch"):
//
//   Prepare   -- SESSION WORKER ONLY. Mutates session_grants_ (lock-free by
//                construction) and BLOCKS on the permission checker, which must
//                never happen on a pool thread: teardown wakes permission
//                waiters via PendingRequests::Shutdown, a mechanism keyed to
//                the worker, and a grant an earlier call acquires must be
//                visible to later calls in the same round, which is exactly
//                the sequential ordering this phase runs in.
//   Execute   -- any thread. Touches no executor state; the tool's shared_ptr
//                rides in the PreparedInvocation.
//
// Run() composes the two on the calling thread and is the whole contract for
// callers that dispatch sequentially (width 1, serial-only tools, tests).
class ToolExecutor {
public:
    // The optional ceilings exist for subagents: a child session may never be
    // more permissive than its parent, but a child preset is resolved from
    // config with no knowledge of who spawns it. The ceiling is the parent's
    // policy, handed down at spawn time and folded in via most_restrictive()
    // AFTER this executor's own resolution and BEFORE apply_run_mode() -- so a
    // per-tool override in the child preset cannot buy back what the parent's
    // corresponding rule forbids. Folding here rather than merging policies up
    // front is what keeps per-tool overrides correct: mode_for() knows the
    // tool's category, and a merged policy no longer does.
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker, PermissionPolicy policy,
                 McpPermissionPolicy mcp_policy, PermissionRunMode run_mode,
                 std::optional<PermissionPolicy> policy_ceiling = std::nullopt,
                 std::optional<McpPermissionPolicy> mcp_ceiling = std::nullopt,
                 std::vector<std::string> seed_grants = {},
                 const hooks::HookDispatcher* hooks = nullptr);

    // NEVER THROWS. A tool_use the loop has already persisted must be answered
    // on every path, or history keeps an orphaned call that both wire formats
    // reject on the next request -- one throwing tool would poison the session
    // for good. Plugin code (category_for, Disclose, execute) reading input the
    // MODEL wrote is where the throws come from; see the comment on Run().
    //
    // GATE-THEN-RUN. Prepare resolves the permission decision for one call --
    // resolve every call of a round first, THEN execute them: prompts still
    // arrive one at a time (better UX than five simultaneous modals), a grant
    // granted mid-round satisfies later calls, and no pool thread can park in
    // the checker. Run() is Prepare+Execute and is all a sequential caller needs.
    [[nodiscard]] PreparedInvocation Prepare(const std::string& tool_use_id,
                                             const std::string& tool_name,
                                             const nlohmann::json& input, ToolContext& ctx);

    // Runs a prepared call. Thread-safe: reads nothing mutable on *this. The
    // ctx passed here belongs to THIS invocation (parallel callers each hold
    // their own). NEVER THROWS, for the same history-integrity reason as Run.
    [[nodiscard]] llm::ToolResultBlock Execute(const PreparedInvocation& prepared,
                                               ToolContext& ctx);

    // Sequential convenience: gate and run on the calling thread.
    llm::ToolResultBlock Run(const std::string& tool_use_id, const std::string& tool_name,
                              const nlohmann::json& input, ToolContext& ctx);

    // Exposed for testing and for surfacing "what would happen" without running.
    PermissionMode ResolveMode(const Tool& tool, const nlohmann::json& input,
                                const ToolContext& ctx) const;

    // True when every key this invocation discloses has already been granted, so
    // the prompt can be skipped. Exposed for testing the invariants directly.
    bool SatisfiedByGrants(const PermissionDisclosure& disclosure) const;

    std::size_t granted_count() const { return session_grants_.size(); }

    // shift+tab's run-mode switch (SetRunModeEvent). The change is queued to
    // the session worker, so it runs between turns rather than beside one --
    // which is what makes a tool dispatch safe without a lock while reading
    // it. The write stays worker-side; the ATOMIC exists because reads are
    // not: EmitSessionMetadata reports it from the RPC pool, and a spawner
    // clamps its child's ceiling against it (clamp_run_mode).
    void SetRunMode(PermissionRunMode mode) {
        run_mode_.store(mode, std::memory_order_relaxed);
    }

    // Session grants are copied into a subagent at spawn -- re-prompting for
    // every binary the parent already approved would make subagents unusable --
    // so the spawning side needs to read them. Sorted for deterministic tests.
    std::vector<std::string> granted_keys() const;

    // The parent's run mode is the ceiling a child's is clamped against
    // (clamp_run_mode), which a spawner cannot do without reading it. Also
    // what the client's {permission_mode} status segment reflects.
    PermissionRunMode run_mode() const { return run_mode_.load(std::memory_order_relaxed); }

    // The policies a spawner hands DOWN as its child's ceilings -- the
    // per-category half of "a child is never more permissive than its parent".
    const PermissionPolicy& policy() const { return policy_; }
    const McpPermissionPolicy& mcp_policy() const { return mcp_policy_; }


private:
    // The real body of the gating phase, split out only so Prepare() can be the
    // one place the catch lives -- an inline try around a function this long
    // invites a later edit to slip a return past it.
    PreparedInvocation PrepareGuarded(const std::string& tool_use_id,
                                      const std::string& tool_name, const nlohmann::json& input,
                                      ToolContext& ctx);

    // ResolveMode with the confinement verdict surfaced: Prepare needs the same
    // classification the out-of-workdir fold used, both for the execute-time
    // re-check and to fold the ceiling's out_of_workdir dimension. Either
    // pointer may be null (the public overload passes null).
    PermissionMode ResolveMode(const Tool& tool, const nlohmann::json& input,
                                const ToolContext& ctx, bool* confinement_checked,
                                bool* was_inside_project_root) const;

    ToolRegistry& registry_;
    PermissionChecker& checker_;
    PermissionPolicy policy_;
    McpPermissionPolicy mcp_policy_;
    std::atomic<PermissionRunMode> run_mode_;  // atomic: see SetRunMode's comment
    std::optional<PermissionPolicy> policy_ceiling_;
    std::optional<McpPermissionPolicy> mcp_ceiling_;

    // Session-scoped and in-memory, deliberately: never written to config or to
    // the session store. A grant's value is a human judging a specific disclosed
    // invocation at that moment; persisting it keeps the shape and discards the
    // substance, which is how a consent affordance decays into an allowlist.
    // ToolExecutor already holds a per-session PermissionPolicy by value, so it
    // is the natural owner.
    std::unordered_set<std::string> session_grants_;

    // The process-wide hook dispatcher (track O). Borrowed, null in tests that
    // exercise only the permission chain: both emissions below are no-ops then.
    // Two points are emitted from here:
    //   core.pre_tool_use      -- SYNC and VETO-CAPABLE, placed after the
    //                             AlwaysDeny short-circuit (a hook never sees a
    //                             call policy already denied -- hooks only
    //                             narrow) and before grants/Ask (a hook deny
    //                             suppresses the prompt).
    //   core.permission_prompted -- observe-only, fired when an Ask is about
    //                             to round-trip to a human.
    const hooks::HookDispatcher* hooks_ = nullptr;
};

// Extracts "<server>" from "mcp__<server>__<tool>". Returns empty for a name
// that is not MCP-namespaced.
std::string mcp_server_of(const std::string& tool_name);

}  // namespace minimal::tools
