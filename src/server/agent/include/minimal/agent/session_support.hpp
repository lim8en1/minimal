#pragma once

// The concrete SessionSupportService. Registered into the ServiceRegistry in
// main.cpp under kSessionSupportServiceName; the INTERFACE header
// (session_support_service.hpp) is what plugins see and the contract that
// governs this file.
//
// LATE WIRING, and the reason it exists: the service object must be Provided
// before ServiceRegistry::Seal() -- that is what puts it inside the startup
// collision scan, so a plugin claiming "core.session_support" is diagnosed as
// the fatal duplicate rather than silently shadowing core. But its
// dependencies (SessionRegistry, the database, the preset registry) are
// serve-stage objects constructed after plugin load. So main.cpp constructs
// this early, Provides it, and calls Wire() in stage 4 once the real objects
// exist -- before the RPC server listens, i.e. before anything can Find() and
// call through it. Unwired, every method degrades to nullopt/null/false.

#include <optional>
#include <string>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/session/session_database.hpp"

namespace minimal::agent {

class SessionSupport final : public SessionSupportService {
public:
    explicit SessionSupport(common::Logger& logger);

    // Serve-stage wiring. Single-threaded (main, before serving starts); no
    // lock needed, and none is taken afterwards either -- the targets are
    // process-wide objects that outlive every session.
    void Wire(SessionRegistry& sessions, session::SessionDatabase& db,
              const AgentPresetRegistry& presets);

    std::optional<SessionLlm> LlmForSession(const std::string& session_id) override;
    std::optional<SessionLlm> ResolveModelRef(const std::string& provider_model) override;
    std::shared_ptr<const session::SessionReader> HistoryFor(
        const std::string& session_id) override;
    bool RenameSession(const std::string& session_id, const std::string& name) override;

private:
    common::Logger& logger_;
    SessionRegistry* sessions_ = nullptr;
    session::SessionDatabase* db_ = nullptr;
    const AgentPresetRegistry* presets_ = nullptr;
    // The DB-backed reader HistoryFor serves when the session is no longer in
    // the registry (see the fallback comments in the .cpp).
    std::shared_ptr<const session::SessionReader> db_reader_;
};

}  // namespace minimal::agent
