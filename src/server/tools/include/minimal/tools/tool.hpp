#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/tools/cancel_token.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::tools {

struct ToolSpec {
    std::string name;
    ToolCategory category;
    llm::ToolDefinition definition;
};

struct ToolResult {
    bool is_error = false;
    std::string content_text;
};

// Lifecycle of a BACKGROUND task -- work a tool starts (background bash) whose
// process outlives the tool call that spawned it. The tool call itself returns
// an acknowledgment; the task reports here as it changes state, and its result
// reaches the model later via ToolContext::schedule_injection.
//
// Core-neutral on purpose: the plugin that runs the task knows nothing about
// gRPC, and the session maps this onto a wire event the same way it maps
// ToolLifecycleEvent. `tool_use_id` is the ORIGINAL invocation -- output deltas
// keep streaming under it, and a client correlates the two.
struct BackgroundTaskEvent {
    enum class State { kStarted, kCompleted, kFailed, kTimedOut, kKilled };
    std::string task_id;
    std::string tool_use_id;
    std::string command;
    int pid = 0;
    State state = State::kStarted;
};

struct ToolContext {
    // The logger is a reference, so it must come from somewhere at construction;
    // the rest is filled in by whoever builds the context.
    explicit ToolContext(common::Logger& logger) : logger(logger) {}

    std::string session_id;
    std::filesystem::path project_root;

    // The invocation this context belongs to, set by ToolExecutor::Run before
    // execute(). A tool that produces a follow-up tied to its own call -- the
    // subagent-spawn completion is injected as a ToolResultBlock keyed to it --
    // needs the id, and no tool can know it any other way.
    std::string tool_use_id;

    // The active preset's allowed_skills for this session, as a plain list so
    // the tools layer stays free of agent-layer types. SkillTool validates its
    // input against it at invocation time; empty means no skills are permitted.
    std::vector<std::string> allowed_skills;

    // Checked before each tool runs and polled by long-running tools (bash).
    std::function<bool()> is_cancelled;

    // The event-driven counterpart, set per turn by the agent loop. Tools that
    // BLOCK on a condition_variable (the MCP connection layer) wait on this
    // token's cv, so cancellation wakes them instead of relying on a poll
    // slice. Null is legal and means "no cancellation path": call sites and
    // tests that never cancel need not mint one. See cancel_token.hpp.
    CancelTokenPtr cancel_token;

    // Read-only access to the transcript this tool runs inside (session::HistoryWindow,
    // a bound, windowed slice of SessionReader). Set by the agent loop per
    // invocation; null when unavailable -- sessions on stores that are not
    // SQLite-backed (test fakes) have no reader behind the window.
    //
    // WINDOWED ACCESS ONLY, and the type enforces it: a tool reads bounded
    // pages through GetMessages, exactly like the GetMessages RPC, and there
    // is no full-materialization path on the window -- LoadAll exists solely
    // for resume and lives on SessionReader, which no tool holds. Nothing is
    // retained here across calls, so the 512k-1M goal is untouched.
    const session::HistoryWindow* history = nullptr;

    // Incremental output, surfaced as ToolOutputDeltaEvent. Deliberately lossy
    // downstream: the server coalesces and sends only the tail that fits the
    // client's ring buffer, while the authoritative record stays the capped
    // ToolResultBlock. Two artifacts, two jobs.
    std::function<void(std::string_view)> emit_output;

    // --- Deferred-completion surface (background bash; see ARCHITECTURE.md) --
    //
    // Three handles a tool captures when it schedules work that OUTLIVES its
    // own invocation. All three are lifetime-safe by construction: the
    // callable's state is the session's shared channels block, and the weak_ptr
    // expires exactly when that block does -- so a detached watcher holding
    // them past the session's death degrades (refusal, expired) instead of
    // dangling. No agent-layer type crosses here.

    // Stages a message for injection at the session's next turn boundary.
    // False means the session is terminating and the message was NOT staged --
    // log the loss, never retry. The message must be wire-valid where it lands
    // (a user message, or a tool-result keyed to a tool_use the history
    // already carries).
    std::function<bool(llm::Message)> schedule_injection;

    // Expires with the session. A long-lived helper polls this (cheaply --
    // expiry is one atomic load on a null-or-live control block) and
    // self-terminates -- for background bash, killpg + reap -- when it does.
    // This is the ONLY session-death signal such a helper gets, and it is
    // deliberately NOT the turn's CancelToken: CancelTurn must not kill
    // background work, termination must.
    std::weak_ptr<const void> session_lifetime;

    // Task lifecycle events (started/completed/failed/timed out/killed).
    // Optional -- a session may wire none -- and safe to call from any thread.
    std::function<void(const BackgroundTaskEvent&)> emit_task_event;

    // Per-invocation audit logging (bash logs command, resolved argv or
    // rejection reason, exit code, duration). One process-wide instance for now.
    common::Logger& logger;
};

class Tool {
public:
    virtual ~Tool() = default;

    virtual const ToolSpec& spec() const = 0;
    virtual ToolResult execute(const nlohmann::json& input, ToolContext& ctx) = 0;

    // Declarative opt-in: a tool whose input carries a filesystem path sets this
    // true and implements ExtractPath(). It never calls path_confinement itself
    // for *classification* -- ToolExecutor does that centrally, so the
    // out-of-workdir rule cannot drift between tools. Bash leaves both at their
    // defaults: there is no sound way to know which paths an arbitrary shell
    // command touches without parsing shell semantics.
    virtual bool path_confinement_supported() const { return false; }
    virtual std::optional<std::string> ExtractPath(const nlohmann::json& input) const {
        (void)input;
        return std::nullopt;
    }

    // Declarative opt-OUT from parallel dispatch (see ARCHITECTURE.md "Tool
    // dispatch"): a tool whose execute() cannot safely overlap another
    // execution of the same tool in this session sets this false. The agent
    // loop then runs its invocations one at a time on the session worker, in
    // assistant-block order, while parallel-safe calls of the same round run on
    // the pool. Default true: a tool that holds no per-session mutable state is
    // safe by construction (ToolContext is per-invocation; providers are
    // stateless by contract).
    //
    // bash opts out: its per-session cwd/env map (owned by the tools-bash
    // plugin, adopted wholesale after each command) would race a concurrent
    // invocation's adopt step. That is a data race the plugin can see, not a
    // semantic ambiguity -- flipping this to true is safe once the plugin
    // serializes adoption internally.
    virtual bool parallel_safe() const { return true; }

    // Overridden only where one tool spans categories -- e.g. Edit's `view`
    // reports ReadOnly though the tool's static category is FileWrite.
    virtual std::optional<ToolCategory> category_for(const nlohmann::json& input) const {
        (void)input;
        return std::nullopt;
    }

    // What an `Ask` prompt should show, and which session grants it may offer.
    // Takes the context because the answer is session-dependent: bash resolves
    // argv[0] against the session's PATH, so `git` may be /usr/bin/git in one
    // session and /tmp/git in another -- and showing the resolved path is the
    // whole point (see GrantOffer).
    //
    // The default -- name only, no grants -- reproduces today's behaviour: every
    // Ask prompts, nothing is ever remembered. A tool opts in by overriding.
    virtual PermissionDisclosure Disclose(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
        (void)input;
        (void)ctx;
        return PermissionDisclosure{spec().name, {}, true};
    }
};

}  // namespace minimal::tools
