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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/logger.hpp"
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
    std::shared_ptr<Session> Create(SessionParams params);

    std::shared_ptr<Session> Find(const std::string& session_id) const;

    // Terminates and forgets. Safe for an id that is already gone.
    void Terminate(const std::string& session_id);

    void TerminateAll();

    std::size_t size() const;

private:
    void ReaperMain();

    const AgentPresetRegistry& presets_;
    tools::ToolRegistry& tools_;
    const llm::ProviderFactoryRegistry& providers_;
    SessionObserverRegistry& observers_;
    session::SessionDatabase& db_;
    LoopConfig loop_config_;
    common::Logger& logger_;
    std::chrono::milliseconds initial_attach_guard_;

    mutable std::mutex mutex_;
    std::condition_variable reaper_cv_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    bool stopping_ = false;
    std::thread reaper_;
};

}  // namespace minimal::agent
