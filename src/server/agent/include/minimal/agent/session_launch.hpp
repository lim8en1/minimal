#pragma once

// Starting a top-level session: create, resume, or fork. The domain rules
// (default preset, project_root validation, row minting and rollback, advisor
// selection, model restore on resume, signal validation) live here; the RPC
// layer only converts its request and maps LaunchError onto a status.
//
// Stateless: every call takes the LaunchContext it needs, borrowed from the
// caller (the RPC service holds all of it for the process lifetime).

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::mcp {
class McpClientManager;
}
namespace minimal::tools {
class PermissionAdvisorRegistry;
}

namespace minimal::agent {

class SessionRegistry;
class SignalRouter;

// Why a launch was refused. The codes line up with the gRPC status the RPC
// layer answers with, but carry no gRPC dependency.
class LaunchError : public std::runtime_error {
public:
    enum class Code {
        kInvalidArgument,     // the request itself is malformed
        kFailedPrecondition,  // well-formed, but the server cannot honour it
        kNotFound,            // no such stored session
        kAlreadyExists,       // the session is already open
        kInternal,            // the store failed
    };
    LaunchError(Code code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    Code code() const { return code_; }

private:
    Code code_;
};

struct LaunchContext {
    SessionRegistry& sessions;
    session::SessionDatabase& db;
    const config::SettingsConfig& settings;
    const config::PermissionsConfig& permissions;
    common::Logger& logger;
    const tools::PermissionAdvisorRegistry* advisors = nullptr;  // null: no advisor applies
    mcp::McpClientManager* mcp_manager = nullptr;  // null: no MCP servers, no liveness checks
    SignalRouter* signal_router = nullptr;         // null: signals are not offered
};

// A new session (session_id empty) or a RESUME of a stored one (session_id
// set). A resume carries only session_id, run_mode, model and signals: the
// stored row governs name, agent_preset and project_root.
struct LaunchRequest {
    std::string session_id;
    std::string name;
    std::string agent_preset;   // empty: settings.default_agent_preset
    std::string project_root;
    std::optional<tools::PermissionRunMode> run_mode;  // unset: the preset's own
    std::string model;          // raw "<provider>/<model>"; empty: none
    std::vector<std::string> signals;
};

struct ForkRequest {
    std::string source_session_id;
    std::int64_t target_seq = 0;
    std::string name;
    std::string model;          // raw "<provider>/<model>"; empty: the source's
    std::string agent_preset;   // empty: the source's
    std::optional<tools::PermissionRunMode> run_mode;
};

// Both return the session's row as stored after the launch. Throw LaunchError.
session::SessionSummary Launch(const LaunchContext& context, const LaunchRequest& request);
session::SessionSummary Fork(const LaunchContext& context, const ForkRequest& request);

}  // namespace minimal::agent
