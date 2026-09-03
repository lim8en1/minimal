#pragma once

// The hook mechanism (track O): NAMED, TYPED HOOK POINTS + config-mapped
// shell actions + one dispatcher.
//
// THREE PARTIES, ONE SEAM:
//
//   Core          registers the conversation-path points (pre_tool_use,
//                 post_tool_use, user_prompt_submit, stop, session_created/
//                 ended, pre/post_compact, subagent_started/completed,
//                 permission_prompted) and emits them from the harness seams.
//
//   Plugins       register ADDITIONAL points through the plugin ABI
//                 (minimal_plugin_register_hook_points) and emit them from
//                 their own code -- the worked example is tools-bash's
//                 before_command. A plugin maps a Block onto its own
//                 semantics; core never interprets a plugin point's payload.
//
//   hooks.json    (pure data) maps shell commands onto points: `on` names the
//                 point, `filter` narrows, `command` runs. Actions are
//                 executed by THIS dispatcher -- plugins never see user hook
//                 config, and the shell executor never sees plugin code.
//
// RULES THE MECHANISM ENFORCES (the plugin or the script cannot bypass them):
//
//   - FAIL-OPEN. A veto-capable point's actions live or die by this: spawn
//     failure, timeout, crash, or exit other than 0/2 behaves as if the
//     action never ran. The permission substrate remains the boundary; a
//     broken hook degrades, never widens.
//
//   - FIRST BLOCK WINS. Actions run in declared order; the first exit-2
//     short-circuits the rest. Running expensive hooks after the decision is
//     made buys nothing.
//
//   - FILTERS MATCH DECLARED FIELDS EXACTLY. A point declares which payload
//     fields are filterable; a filter referencing anything else is a
//     startup warning. Never substring matching, never model-controlled
//     input contents.
//
//   - HOOKS ONLY NARROW. Emission sites sit below the substrate's
//     AlwaysDeny/run-mode short-circuits, so a hook can deny what policy
//     allowed and can never allow what policy denied. (The dispatcher cannot
//     enforce this one -- the composition sites own it -- which is why each
//     site's test asserts it.)
//
// CONCURRENCY: registration and Configure are startup-only (single-threaded,
// before serving). Emit runs on session workers -- the map is read-only by
// then, unguarded like ServiceRegistry. EmitAsync posts to ONE dedicated
// thread (the SessionObserverRegistry pattern: ordered, throw-contained,
// fire-and-forget, drop-counting) so observe-only points can never block a
// session worker, and a hung async action stalls further notifications rather
// than the harness.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/common/event_queue.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/hooks_config.hpp"
#include "minimal/llm/chat.hpp"

#include <nlohmann/json.hpp>

namespace minimal::hooks {

// Transparent hashing so Emit can take a string_view without materializing a
// std::string per lookup.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

// Points registered by the harness itself. Names are qualified by owner, the
// same convention as services ("plugin.point"); core's prefix is a namespace,
// and a plugin registering a core.* name is an ordinary fatal collision.
inline constexpr char kPreToolUse[] = "core.pre_tool_use";
inline constexpr char kPostToolUse[] = "core.post_tool_use";
inline constexpr char kUserPromptSubmit[] = "core.user_prompt_submit";
inline constexpr char kStop[] = "core.stop";
inline constexpr char kSessionCreated[] = "core.session_created";
inline constexpr char kSessionEnded[] = "core.session_ended";
inline constexpr char kPreCompact[] = "core.pre_compact";
inline constexpr char kPostCompact[] = "core.post_compact";
inline constexpr char kSubagentStarted[] = "core.subagent_started";
inline constexpr char kSubagentCompleted[] = "core.subagent_completed";
inline constexpr char kPermissionPrompted[] = "core.permission_prompted";

// Stable wire-style names for a stop reason: hooks.json filters key on these
// strings, so they are surface, not diagnostics. One definition, shared by
// every emitter that carries a stop_reason payload field.
inline const char* StopReasonName(llm::StopReason reason) {
    switch (reason) {
        case llm::StopReason::EndTurn: return "end_turn";
        case llm::StopReason::ToolUse: return "tool_use";
        case llm::StopReason::MaxTokens: return "max_tokens";
        case llm::StopReason::StopSequence: return "stop_sequence";
        case llm::StopReason::ContentFilter: return "content_filter";
        case llm::StopReason::Refusal: return "refusal";
        case llm::StopReason::Cancelled: return "cancelled";
        case llm::StopReason::Unknown: return "unknown";
    }
    return "unknown";
}

struct HookPointSpec {
    std::string name;
    // False = observe-only: the dispatcher ignores a block verdict (and the
    // sanity check warns about an action whose only meaningful exit is one
    // this point cannot answer). True = Emit returns the block reason.
    bool veto_capable = false;
    // Payload fields a hooks.json filter may reference. Enforced at Configure;
    // a filter naming anything else is a startup warning. Frozen surface for
    // shell consumers -- a renamed field silently detaches a user's filter,
    // so changes here are API changes.
    std::vector<std::string> filterable;
};

// The emitter's handle, returned by RegisterPoint. Copyable; validity is
// process-lifetime (points are never unregistered, plugin handles are never
// dlclosed). A default-constructed handle is DISABLED: Emit passes, EmitAsync
// drops -- which is what a test or a missing dispatcher should look like.
class HookDispatcher;

class HookPoint {
public:
    HookPoint() = default;

    const HookPointSpec& spec() const { return spec_; }
    bool enabled() const { return dispatcher_ != nullptr; }

    // Synchronous. nullopt = passed; a value = block reason (veto-capable
    // points only). Blocks the calling thread for at most one action's
    // timeout per mapped action; never throws.
    std::optional<std::string> Emit(
        nlohmann::json payload,
        const std::function<bool()>& is_cancelled = {}) const;

    // Fire-and-forget. Never blocks the caller; results (including blocks)
    // are logged, not returned.
    void EmitAsync(nlohmann::json payload) const;

private:
    friend class HookDispatcher;
    HookPoint(const HookDispatcher* dispatcher, HookPointSpec spec)
        : dispatcher_(dispatcher), spec_(std::move(spec)) {}

    const HookDispatcher* dispatcher_ = nullptr;
    HookPointSpec spec_;
};

class HookDispatcher {
public:
    explicit HookDispatcher(common::Logger& logger);
    ~HookDispatcher();

    HookDispatcher(const HookDispatcher&) = delete;
    HookDispatcher& operator=(const HookDispatcher&) = delete;

    // --- Registration (startup only, single-threaded) ------------------------

    // Registers a point and returns the emitter's handle. A duplicate name is
    // NOT silently shadowed: the attribution pair lands in attributions()
    // twice, which the startup name-collision check reports as fatal (the
    // ServiceRegistry pattern), and the FIRST registration stays bound.
    // Empty attribution_ marks a registration made by core itself.
    HookPoint RegisterPoint(HookPointSpec spec);

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Attribution {
        std::string plugin_name;  // empty for registrations made by core itself
        std::string point_name;   // the FULL point name, e.g. "tools-bash.before_command"
    };
    const std::vector<Attribution>& attributions() const { return attributions_; }

    // --- Configuration (once, after plugin load; seals registration) ---------

    // Resolves hooks.json actions onto registered points and seals the point
    // map: from here on the map is read-only, which is what makes unguarded
    // Emit from session workers safe. Warnings (unknown point names, filter
    // fields the point does not declare, actions on non-veto points) are
    // logged AND returned, so the startup path can surface them like the rest
    // of the sanity check's findings.
    std::vector<std::string> Configure(const config::HooksConfig& config);

    // Structural warnings for one hooks.json action against currently-
    // registered points -- exactly what Configure checks when it seals,
    // without mutating anything. Shared with the startup sanity check's
    // check_hooks() so the two never drift (in particular, the "no filter --
    // answers EVERY emission" warning, which used to exist only here).
    std::vector<std::string> DescribeActionWarnings(const config::HookActionConfig& action) const;

    // --- Emission -------------------------------------------------------------

    // Synchronous dispatch for veto-capable points. nullopt = passed (also:
    // unknown point, no mapped actions, or all actions passed). Never throws.
    std::optional<std::string> Emit(
        std::string_view point, nlohmann::json payload,
        const std::function<bool()>& is_cancelled = {}) const;

    // Fire-and-forget dispatch for observe-only points. Ordered on one
    // dedicated thread; a full queue DROPS and counts (an observe-only hook
    // must never backpressure a session), like SessionObserverRegistry.
    void EmitAsync(std::string_view point, nlohmann::json payload) const;

    std::size_t dropped_notifications() const { return dropped_.load(std::memory_order_acquire); }

    const HookPointSpec* Find(std::string_view point) const;

    // Testing seam. Blocks until every queued async task has run.
    void WaitForIdleForTesting();

    static constexpr std::size_t kDefaultTaskQueueSize = 1024;

private:
    struct Action {
        std::string command;
        std::chrono::milliseconds timeout{10000};
        // (field, expected value), ANDed, exact match on string payload fields.
        std::vector<std::pair<std::string, std::string>> filter;
    };

    struct PointEntry {
        HookPointSpec spec;
        std::vector<Action> actions;  // declared order; first Block wins
    };

    const PointEntry* FindEntry(std::string_view point) const;
    // Runs every mapped action that passes the filter, in order. Returns the
    // first block reason, or nullopt.
    std::optional<std::string> RunActions(const PointEntry& entry, nlohmann::json payload,
                                          const std::function<bool()>& is_cancelled) const;
    static bool FilterMatches(const Action& action, const nlohmann::json& payload);
    static nlohmann::json Envelope(std::string_view point, nlohmann::json payload);

    common::Logger& logger_;
    std::unordered_map<std::string, PointEntry, TransparentStringHash, std::equal_to<>> points_;
    std::vector<Attribution> attributions_;
    std::string attribution_;
    bool sealed_ = false;

    // The async dispatch thread. Runs from construction (the queue parks it)
    // so Configure needs no thread-start choreography; Close + join in the
    // destructor. Mutable because Emit/EmitAsync are const: they run after
    // Configure sealed the ACTION map, but the queue and counters are runtime
    // state by design.
    mutable common::EventQueue<std::function<void()>> tasks_;
    mutable std::thread dispatcher_thread_;
    mutable std::atomic<int> outstanding_{0};
    mutable std::atomic<std::size_t> dropped_{0};
};

}  // namespace minimal::hooks
