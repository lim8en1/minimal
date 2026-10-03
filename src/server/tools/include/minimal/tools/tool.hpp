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
#include "minimal/tools/prompt.hpp"
#include "minimal/tools/background_task_event.hpp"
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
    // Optional display payload for clients: {"kind": "<kind>", ...}. Never
    // sent to the model -- content_text stays what the model reads and the
    // fallback a client shows when it has no renderer for the kind. Null =
    // absent. Self-contained: a renderer never parses content_text.
    nlohmann::json display = nullptr;
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

    // The active preset's skill access for this session, as plain data so the
    // tools layer stays free of agent-layer types. SkillTool validates its
    // input against it at invocation time; empty means no skills are permitted.
    // `ask` marks a skill loadable only after a permission prompt (opencode's
    // `ask` mode) -- the skill tool narrows its own resolution for those via
    // mode_override_for. Preloaded skills are NOT listed here: their bodies
    // already sit in the system prompt, and calling the tool for them is a
    // no-op the prompt forbids.
    struct SkillAccess {
        std::string name;
        bool ask = false;
    };
    std::vector<SkillAccess> allowed_skills;

    // The active preset's slice of `tool_config` for THIS tool (its
    // config_namespace() key in agents/*.md frontmatter), as plain data so the
    // tools layer stays free of agent-layer types -- the same pattern as
    // allowed_skills above. Null when the tool declares no namespace or the
    // preset declares no slice for it. Points into the agent loop's preset,
    // which is stable for the turn (preset swaps happen on the worker between
    // turns), so a tool may retain the pointer for its invocation's duration
    // -- background watchers included, which hold the ctx handles anyway.
    // Core never interprets the contents: the plugin owns its slice's schema
    // and its merge with its own plugins.json config.
    const nlohmann::json* tool_config = nullptr;

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

    // --- Plugin prompt (see prompt.hpp; the tool-ask plugin's `ask_user`) ---
    //
    // Blocks until a human answers the plugin-shaped prompt, then returns the
    // outcome and the (unvalidated) answer. Optional -- a session may wire no
    // prompt broker (headless fixtures, tests) -- and null means "host offers
    // no interactive prompt", which a tool treats as PromptOutcome::
    // Unavailable rather than a hang. Not safe to call from a detached
    // thread: it parks on the session's PendingRequests, so it must run where
    // blocking is allowed, exactly like a permission prompt. Its lifetime
    // discipline is the session's channels block, like the handles above.
    std::function<PromptResult(const PromptRequest&)> prompt_user;

    // Per-FILE permission for a tool that reads files its call never named --
    // grep/glob walking a tree. Permission gating resolves the call's own path
    // (the walk ROOT), so without this an operator's `"**/secrets/**": deny`
    // stopped grep(path="secrets") but not grep(path="."), which read the
    // same files anyway. True = the operator's path rules allow this file
    // under the call's approval: its mode is no stricter than the call's own.
    // A file an `ask` pattern covers is NOT permitted -- a walk cannot prompt
    // per file, so naming the file directly (and being asked) stays the only
    // way in. `absolute` is canonical (the walker never follows symlinks
    // below a canonical root). Set by ToolExecutor::Execute for the duration
    // of execute(); null = no per-path rules apply, everything is permitted.
    std::function<bool(const std::filesystem::path& absolute)> path_permitted;

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

    // The string permission PATTERN rules match against (GlobMatch; see
    // PermissionPolicy::pattern_overrides). Default: the ExtractPath result
    // for confinement-supported tools -- the executor canonicalizes it before
    // matching. A tool whose subject is not a path overrides this; bash
    // returns the command line (the glob flavor differs accordingly -- the
    // executor knows which subject is a path). nullopt = no subject for this
    // input, so no pattern can match.
    virtual std::optional<std::string> PatternSubject(const nlohmann::json& input) const {
        return path_confinement_supported() ? ExtractPath(input) : std::nullopt;
    }

    // Per-stage subjects for tools whose single subject is a compound of
    // independently-ruleable parts (bash: the pipeline/list stages). Default:
    // none -- the executor then matches PatternSubject alone. Stages ride the
    // SAME glob rules as the whole-line subject; the executor decides how a
    // rule interacts with the set (see permission.hpp PatternSubjects). A
    // wrapper must forward this like every other declarative virtual.
    virtual std::optional<std::vector<std::string>> PatternStages(
        const nlohmann::json& input) const {
        (void)input;
        return std::nullopt;
    }

    // Extra subjects for ask/deny pattern rules ONLY -- alternate spellings of
    // the stages a rule author means by the same program (bash: `/bin/rm x`,
    // `'rm' x`, `sudo rm x` all also present as `rm x`). Never consulted by
    // allow rules, so a tool can only make itself stricter here. Default:
    // none. A wrapper must forward this like PatternStages.
    virtual std::vector<std::string> PatternRestrictSubjects(const nlohmann::json& input) const {
        (void)input;
        return {};
    }

    // Whether allow-mode pattern rules may act on this invocation's stages. A
    // tool whose decomposition can be shaky or can hide a side effect the
    // stages do not name (bash: an unparseable program, an output redirect)
    // returns false, which keeps allow rules inert -- ask/deny rules still match
    // -- so the category default stands. Default true. A wrapper must forward
    // this like PatternStages.
    virtual bool PatternAllowEligible(const nlohmann::json& input) const {
        (void)input;
        return true;
    }

    // Declarative opt-in, same shape as path_confinement_supported(): a tool
    // that reaches the network (S3's web_fetch/web_search) sets this true so
    // ToolExecutor folds in PermissionPolicy::network via most_restrictive.
    // Default false: most tools (bash, the fs tools, search) never leave the
    // local machine, and tagging them would fold in a dimension that has
    // nothing to do with what they actually do.
    virtual bool network_access_required() const { return false; }

    // Declarative opt-in, same shape as path_confinement_supported(): a tool
    // that reads/writes/edits file CONTENT a human would not want an agent
    // silently touching (.env and friends) sets this true so
    // tools::default_pattern_rules() denies dotenv paths for it by default --
    // via ToolRegistry::DotenvProtectedPermissionKeys(), without core ever
    // needing to know the tool's name. Default false: most tools (glob, grep,
    // bash) either never touch file content or have no path-shaped subject
    // this protects.
    virtual bool default_dotenv_protected() const { return false; }

    // Declarative opt-in for per-preset plugin config: the namespace this tool
    // reads from the active preset's `tool_config` frontmatter map
    // (agents/*.md: `tool_config: {<namespace>: {...}}`). Delivered
    // per-invocation as ToolContext::tool_config -- THIS tool's slice only, or
    // null when the preset declares none. Default empty: a tool that reads no
    // per-preset config never sees anyone's slice. Two plugins claiming the
    // same namespace is a FATAL startup collision (the NameRegistry discipline,
    // one level up); several tools of ONE plugin sharing a namespace is normal.
    // See ARCHITECTURE.md "Server plugins" and docs/PLUGINS.md for the author
    // contract (the plugin owns its slice's schema and its merge with its own
    // plugins.json config).
    virtual std::string_view config_namespace() const { return {}; }

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

    // Per-invocation permission NARROWING for tools whose danger depends on the
    // ARGUMENTS rather than the tool (the skill tool: which skill body it is
    // about to hand the model). Folded into ResolveMode as most_restrictive --
    // it can only tighten, never widen -- and folded BEFORE the run mode so
    // Bypass still washes an Ask back to allow and AllowOnly still clamps it to
    // deny. Default: no narrowing.
    virtual std::optional<PermissionMode> mode_override_for(const nlohmann::json& input,
                                                            const ToolContext& ctx) const {
        (void)input;
        (void)ctx;
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
