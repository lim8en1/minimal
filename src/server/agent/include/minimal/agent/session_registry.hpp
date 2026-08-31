#pragma once

// Owns every live session, and runs the initial-attach guard.
//
// THE GUARD IS A SAFETY NET, NOT A DESIGNED FLOW. Attaching to a session --
// including a subagent child announced by SubagentStartedEvent -- is a client
// protocol obligation. The rule is not "unattached => terminate", because a
// subagent emits events before its tab attaches; it is "never-yet-attached =>
// terminate after a short grace period", which only fires against a client that
// failed its obligation. Once a session has been attached, stream drop is what
// terminates it. See ARCHITECTURE.md "Session lifetime".

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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

class SessionRegistry {
public:
    SessionRegistry(const AgentPresetRegistry& presets, tools::ToolRegistry& tools,
                    const llm::ProviderFactoryRegistry& providers,
                    SessionObserverRegistry& observers, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger,
                    std::chrono::milliseconds initial_attach_guard = std::chrono::seconds(10));
    ~SessionRegistry();

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

    // The process-wide hook dispatcher (track O). Optional and borrowed; set
    // before any session exists. Attached to every params.hooks that arrives
    // null -- the funnel property, like the history reader below.
    void set_hook_dispatcher(const hooks::HookDispatcher* hooks) { hooks_ = hooks; }

    std::shared_ptr<Session> Create(SessionParams params);

    // Same as Create(), but the caller supplies the ALREADY RESOLVED preset
    // instead of having it looked up by params.agent_preset. Subagent spawning
    // is the consumer: the spawner adapts the resolved preset first (is_subagent
    // set, subagent.md prefixed onto the system prompt, allowed lists
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

    // Live sessions whose parent_session_id is `parent_id`. The subagent
    // concurrency limit is a count of these.
    std::size_t ChildCount(const std::string& parent_id) const;

    // Terminates and forgets. Safe for an id that is already gone.
    void Terminate(const std::string& session_id);

    void TerminateAll();

    std::size_t size() const;

private:
    void ReaperMain();

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
    common::Logger& logger_;
    std::chrono::milliseconds initial_attach_guard_;

    mutable std::mutex mutex_;
    std::condition_variable reaper_cv_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    bool stopping_ = false;
    std::thread reaper_;
};

}  // namespace minimal::agent
