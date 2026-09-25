#pragma once

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/tools/permission_model.hpp"
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
    //
    // `models` are the plugin permission models bound to this session's
    // preset (permission_model.hpp); `models_ceiling` the parent's, bound to
    // the parent's preset, folded exactly like `policy_ceiling`.
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker, PermissionPolicy policy,
                 PermissionModels models, PermissionRunMode run_mode,
                 std::optional<PermissionPolicy> policy_ceiling = std::nullopt,
                 std::optional<PermissionModels> models_ceiling = std::nullopt,
                 std::vector<std::string> seed_grants = {},
                 const hooks::HookDispatcher* hooks = nullptr);

    // The form for callers still holding an McpPermissionPolicy: binds it to
    // the in-core MCP model. A template only so a braced `{}` fourth argument
    // is not ambiguous with the form above (it cannot deduce M, so `{}` means
    // PermissionModels). Goes away when the MCP client leaves core.
    template <typename M, std::enable_if_t<std::is_same_v<M, McpPermissionPolicy>, int> = 0>
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker, PermissionPolicy policy,
                 M mcp_policy, PermissionRunMode run_mode,
                 std::optional<PermissionPolicy> policy_ceiling = std::nullopt,
                 std::optional<McpPermissionPolicy> mcp_ceiling = std::nullopt,
                 std::vector<std::string> seed_grants = {},
                 const hooks::HookDispatcher* hooks = nullptr)
        : ToolExecutor(registry, checker, std::move(policy),
                       PermissionModels{BindMcpPermissionModel(std::move(mcp_policy))}, run_mode,
                       std::move(policy_ceiling),
                       mcp_ceiling ? std::optional<PermissionModels>(PermissionModels{
                                         BindMcpPermissionModel(std::move(*mcp_ceiling))})
                                   : std::nullopt,
                       std::move(seed_grants), hooks) {}

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

    // The gate's whole verdict for one invocation: the final mode and the rule
    // that decided it, in the words a denial and a prompt show the operator
    // ("preset: file_write=deny", "pattern `git push *`=ask", "outside the
    // project: out_of_workdir=ask", "parent session: ...", "run mode
    // allow-only turns ask into deny"). Pure: no prompt, no hook, no grant --
    // which is also what makes it the /permissions dry run.
    struct Verdict {
        PermissionMode mode = PermissionMode::Ask;
        std::string reason;
        // False when an operator-written `ask` pattern (this policy's or the
        // ceiling's) matched: that Ask always reaches the checker, grants or not.
        bool ask_grantable = true;
        // The confinement classification the out-of-workdir fold used, for
        // Execute's re-check. checked=false when the fold never ran.
        bool confinement_checked = false;
        bool inside_project_root = false;
    };
    Verdict Explain(const Tool& tool, const nlohmann::json& input, const ToolContext& ctx) const;

    // Explain by tool name, for the /permissions dry run: nullopt when no such
    // tool is registered. `grants_satisfy` (optional) reports whether the
    // grants held right now would answer a resulting Ask without a prompt --
    // the same test Prepare runs (disclosure conjunction + ask_grantable).
    // SESSION WORKER ONLY: it reads the grant set Prepare mutates.
    std::optional<Verdict> Explain(const std::string& tool_name, const nlohmann::json& input,
                                   const ToolContext& ctx, bool* grants_satisfy = nullptr) const;

    // Exposed for testing and for surfacing "what would happen" without running.
    PermissionMode ResolveMode(const Tool& tool, const nlohmann::json& input,
                                const ToolContext& ctx) const {
        return Explain(tool, input, ctx).mode;
    }

    // True when every key this invocation discloses has already been granted, so
    // the prompt can be skipped. Exposed for testing the invariants directly.
    bool SatisfiedByGrants(const PermissionDisclosure& disclosure) const;

    std::size_t granted_count() const { return session_grants_.size(); }

    // shift+tab's run-mode switch (SetRunModeEvent), applied by
    // Session::SetRunMode on the READER thread the moment the event arrives
    // -- the mode must be able to land mid-turn, at the very next gated tool
    // call, and a queued work item would wait out the whole turn for nothing.
    // The write is as safe cross-thread as the reads it serves: this is a
    // single atomic enum with no dependent data, and every reader (tool
    // dispatch, a spawner's clamp, metadata emission) already loads it
    // lock-free. `relaxed` is deliberate -- the value has no ordering
    // relationship with any other write.
    void SetRunMode(PermissionRunMode mode) {
        run_mode_.store(mode, std::memory_order_relaxed);
    }

    // A preset switch (/agent, /model re-resolution, a per-turn borrow and its
    // restore) is a POLICY swap: without this the executor kept enforcing the
    // policy of whichever preset the session was CREATED with, so `/agent plan`
    // from build still asked for writes plan denies. SESSION WORKER ONLY, at a
    // turn boundary or turn pop -- the same thread and ordering Prepare runs on,
    // so no lock is needed. Ceilings, grants and the run mode are untouched: a
    // subagent's ceiling is its parent's and a preset switch never lifts it.
    void SetPolicies(PermissionPolicy policy, PermissionModels models) {
        policy_ = std::move(policy);
        models_ = WithMcpModel(std::move(models));
    }
    template <typename M, std::enable_if_t<std::is_same_v<M, McpPermissionPolicy>, int> = 0>
    void SetPolicies(PermissionPolicy policy, M mcp_policy) {
        SetPolicies(std::move(policy), PermissionModels{BindMcpPermissionModel(std::move(mcp_policy))});
    }

    // The labels every tool hook payload carries (core.pre_tool_use,
    // core.permission_prompted, core.post_tool_use) -- one source, so a
    // `permission_key`/`category` hook filter means the same thing at every
    // point. category is the input-sensitive one (edit's `view` is
    // read_only) and "mcp" for MCP tools. Thread-safe: reads the registry
    // under its own lock and the tool's const surface only. A tool no longer
    // registered (a replaced MCP server) keeps its permission key and gets
    // the category "unknown" unless it is MCP-namespaced.
    struct HookLabels {
        std::string permission_key;
        std::string category;
    };
    HookLabels hook_labels(const std::string& tool_name, const nlohmann::json& input) const;

    // Session grants are copied into a subagent at spawn -- re-prompting for
    // every binary the parent already approved would make subagents unusable --
    // so the spawning side needs to read them. Sorted for deterministic tests.
    std::vector<std::string> granted_keys() const;

    // /permissions revoke: drops one held grant (true if it was held), or all
    // of them. SESSION WORKER ONLY, like every other grant-set mutation.
    // Children already spawned keep the copy they were seeded with.
    bool RevokeGrant(const std::string& key) { return session_grants_.erase(key) != 0; }
    std::size_t RevokeAllGrants() {
        const std::size_t count = session_grants_.size();
        session_grants_.clear();
        return count;
    }

    // The parent's run mode is the ceiling a child's is clamped against
    // (clamp_run_mode), which a spawner cannot do without reading it. Also
    // what the client's {permission_mode} status segment reflects.
    PermissionRunMode run_mode() const { return run_mode_.load(std::memory_order_relaxed); }

    // The policies a spawner hands DOWN as its child's ceilings -- the
    // per-category half of "a child is never more permissive than its parent".
    const PermissionPolicy& policy() const { return policy_; }
    const PermissionModels& permission_models() const { return models_; }
    // The MCP model's layer, or an all-default policy when none is bound.
    const McpPermissionPolicy& mcp_policy() const;


private:
    // While MCP is core, every executor enforces the MCP model: a set without
    // an MCP binding gets the all-default one (per-server Ask), so an mcp__
    // tool can never fall through to the core model's category default.
    static PermissionModels WithMcpModel(PermissionModels models);

    // The real body of the gating phase, split out only so Prepare() can be the
    // one place the catch lives -- an inline try around a function this long
    // invites a later edit to slip a return past it.
    PreparedInvocation PrepareGuarded(const std::string& tool_use_id,
                                      const std::string& tool_name, const nlohmann::json& input,
                                      ToolContext& ctx);


    ToolRegistry& registry_;
    PermissionChecker& checker_;
    PermissionPolicy policy_;
    PermissionModels models_;
    std::atomic<PermissionRunMode> run_mode_;  // atomic: see SetRunMode's comment
    std::optional<PermissionPolicy> policy_ceiling_;
    std::optional<PermissionModels> models_ceiling_;

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

}  // namespace minimal::tools
