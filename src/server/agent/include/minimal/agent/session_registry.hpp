#pragma once

// Owns every live session, and runs the lifetime rules: the initial-attach
// guard, the detached-idle reaper, and the process idle exit.
//
// THE GUARD IS A SAFETY NET, NOT A DESIGNED FLOW. Attaching to a session --
// including a subagent child announced by SubagentStartedEvent -- is a client
// protocol obligation. The rule is not "unattached => terminate", because a
// subagent emits events before its tab attaches; it is "never-yet-attached =>
// terminate after a short grace period", which only fires against a client that
// failed its obligation. Once a session has been attached, stream drop is what
// DETACHES it (S5): the session outlives its clients and is collected by the
// detached-idle reaper only after it has been fully quiescent for
// sessions.detached_idle_ttl_seconds. See ARCHITECTURE.md "Session lifetime".

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/provider_instance_cache.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

// Thrown by Create/CreateResolved when params.session_id names a session that
// is already open -- or is being constructed RIGHT NOW by a concurrent call.
// The registry reserves the id under its mutex before any construction work
// runs, so the loser fails BEFORE the transcript load, the torn-tail repair
// append, or any other side effect. The RPC layer maps this to ALREADY_EXISTS
// (a duplicate resume is the client's doing) rather than FAILED_PRECONDITION
// (which it reserves for configuration errors).
class SessionAlreadyOpen : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionRegistry {
public:
    // S5 lifetime policy, settable after construction (main reads settings
    // once and wires the idle-exit halves it owns). Every duration here is a
    // settings.json value (SessionSettings / ServerSettings) with a
    // documented default; nothing at the use site is hardcoded.
    struct ReaperConfig {
        // The never-attach guard (sessions.initial_attach_guard_seconds).
        std::chrono::milliseconds initial_attach_guard = std::chrono::seconds(10);
        // A fully quiescent detached session (no subscribers, no turn in
        // flight, no live background tasks, no subagent children) is
        // terminated after being quiescent this long. Collecting a quiescent
        // session is lossless: the transcript stays in SQLite, and re-attach
        // becomes resume. 0 = never reap detached sessions.
        std::chrono::seconds detached_idle_ttl = std::chrono::seconds(1800);
        // Once zero clients are attached AND zero sessions are live, fire
        // on_idle_expired after this linger. 0 = never (daemon deployment).
        std::chrono::seconds idle_shutdown = std::chrono::seconds(5);
        // The service's attached-stream count; empty disables the idle exit.
        std::function<std::size_t()> attached_clients;
        // Wired to Server::RequestShutdown by main. Fired from the reaper
        // thread; must not block.
        std::function<void()> on_idle_expired;
    };

    SessionRegistry(const AgentPresetRegistry& presets, tools::ToolRegistry& tools,
                    const llm::ProviderFactoryRegistry& providers,
                    SessionObserverRegistry& observers, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger,
                    std::chrono::milliseconds initial_attach_guard = std::chrono::seconds(10));
    ~SessionRegistry();

    // Replaces the lifetime policy before any session exists (or after -- the
    // reaper reads it under the mutex each tick). The constructor's
    // initial_attach_guard parameter stays for call sites that never touch
    // this; setting it overrides the guard too.
    void set_reaper_config(ReaperConfig config);

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // The session row must already exist in the database -- params.session_id
    // names it -- because the database is what mints session ids. See
    // SessionServiceImpl::CreateSession, which owns that ordering.
    //
    // Throws std::runtime_error naming the problem: an unknown preset, or a
    // providers.json entry whose `provider` kind no loaded plugin registered.
    // Both are configuration errors the caller must be able to report back as
    // such, not conditions to paper over with a null provider.
    // Optional: sessions created without one answer every command invocation
    // with "not available". Set before any session exists.
    void set_commands(const commands::CommandRegistry* commands) { commands_ = commands; }

    // Project context (AGENTS.md/CLAUDE.md) settings, read once at startup.
    // Optional but DEFAULTS TO DISABLED: until main wires the parsed settings
    // in, no session reads anything from its project_root -- the same shape as
    // set_commands, where a missing producer is a feature that is off rather
    // than one running on guessed defaults. Loading happens per session in
    // CreateResolved -- the one funnel top-level creation, resume, and
    // SubagentManager spawns share -- because the file lives in the SESSION's
    // project_root, not in any preset.
    void set_project_context(config::ProjectContextSettings settings) {
        project_context_ = settings;
    }

    // sessions.unattended_prompt_timeout_seconds, read once at startup like
    // project_context above and applied through the same CreateResolved
    // funnel. Zero (the default) means every prompt waits forever, unchanged
    // from before S5.
    void set_prompt_timeout(std::chrono::milliseconds timeout) {
        prompt_timeout_ = timeout;
    }

    // The process-wide hook dispatcher (track O). Optional and borrowed; set
    // before any session exists. Attached to every params.hooks that arrives
    // null -- the funnel property, like the history reader below.
    void set_hook_dispatcher(const hooks::HookDispatcher* hooks) { hooks_ = hooks; }

    std::shared_ptr<Session> Create(SessionParams params);

    // Same as Create(), but the caller supplies the ALREADY RESOLVED preset
    // instead of having it looked up by params.agent_preset. Subagent spawning
    // is the consumer: the spawner adapts the resolved preset first (is_subagent
    // set, prompts/subagent.md prefixed onto the system prompt, allowed lists
    // intersected with the parent's), and no name-based lookup could express
    // that. params.agent_preset still names the preset for the database row and
    // for SessionInfo.
    //
    // max_turns_override bounds the child's tool rounds (a subagent's whole life
    // is one RunTurn); nullopt keeps the registry-wide LoopConfig value, which
    // is 0 -- unbounded -- unless the spawner says otherwise.
    std::shared_ptr<Session> CreateResolved(SessionParams params, AgentPreset preset,
                                            std::optional<int> max_turns_override = std::nullopt);

    std::shared_ptr<Session> Find(const std::string& session_id) const;

    // The shared provider instance cache. Borrowed by SessionSupportService's
    // model-override resolution (a titler resolving a configured
    // "<provider>/<model>" lands in the same cache session construction uses,
    // so the instance is shared, never duplicated). The registry outlives
    // every session and every service call.
    ProviderInstanceCache& provider_cache() { return provider_cache_; }

    // Sessions of `parent_id` that are still RUNNING: neither terminated nor
    // finished-with-their-seeded-turn. This is the count the subagent
    // concurrency limit admits against -- a finished child whose tab is still
    // open holds no slot. Called under SubagentManager::limit_mu_ (lock order
    // limit_mu_ -> mutex_, one-directional; see subagent_manager.hpp).
    std::size_t ChildCount(const std::string& parent_id) const;

    // Terminates and forgets. Safe for an id that is already gone.
    void Terminate(const std::string& session_id);

    void TerminateAll();

    std::size_t size() const;

private:
    void ReaperMain();
    // The lock-already-held twin of ChildCount(), for ReaperMain -- which
    // holds mutex_ across its whole sweep. Calling the public ChildCount()
    // from there would relock a non-recursive std::mutex on the same thread
    // and deadlock the reaper permanently, wedging every subsequent Find()/
    // size()/etc. behind a lock nobody will ever release.
    std::size_t ChildCountLocked(const std::string& parent_id) const;

    const AgentPresetRegistry& presets_;
    // Optional and borrowed; every session gets the same one.
    const commands::CommandRegistry* commands_ = nullptr;
    tools::ToolRegistry& tools_;
    const llm::ProviderFactoryRegistry& providers_;
    // Shared provider instances across every session this registry creates.
    // Declared after providers_ so the factory registry outlives the cache.
    ProviderInstanceCache provider_cache_;
    SessionObserverRegistry& observers_;
    // Optional and borrowed; every session gets the same one via the
    // CreateResolved funnel.
    const hooks::HookDispatcher* hooks_ = nullptr;
    session::SessionDatabase& db_;
    LoopConfig loop_config_;
    config::ProjectContextSettings project_context_ = [] {
        config::ProjectContextSettings disabled;
        disabled.enabled = false;
        return disabled;
    }();
    std::chrono::milliseconds prompt_timeout_{0};
    common::Logger& logger_;
    std::chrono::milliseconds initial_attach_guard_;
    ReaperConfig reaper_config_;
    // When each session was FIRST observed detached-and-quiescent (the
    // reaper-local half of the idle-TTL rule; the session itself carries no
    // timestamps for it). Erased whenever a session is active or attached.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> quiescent_since_;
    // When the process first observed zero clients and zero sessions.
    std::optional<std::chrono::steady_clock::time_point> idle_since_;

    mutable std::mutex mutex_;
    std::condition_variable reaper_cv_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    // Ids claimed by an in-flight Create/CreateResolved, before any
    // construction work has run. The old guard was a check at the END of
    // construction -- after the resume transcript load, the torn-tail repair
    // append, and the worker start -- so two concurrent resumes both did the
    // full work before the loser was refused. A reservation under the same
    // mutex makes the refusal the FIRST thing that happens.
    std::unordered_set<std::string> reserved_;
    bool stopping_ = false;
    std::thread reaper_;
};

}  // namespace minimal::agent
