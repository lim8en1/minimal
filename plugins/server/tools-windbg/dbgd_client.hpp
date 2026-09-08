#pragma once

// The dbgd wire client: the slice of dbg-cli's protocol this tool speaks,
// over the core's net::HttpClient. dbgd is the SIDECAR here -- one server
// process owns one debugger engine (WinDbg/DbgEng via pybag, lldb, ...), and
// this client is deliberately engine-ignorant: it knows sessions, exec
// (with the ack-within-grace -> job pattern), job long-poll reads, the
// lease-free interrupt, and teardown. All of the hard debugger problems
// (the one-WaitForEvent rule, worker isolation, KDNET/dbgsrv remoting) stay
// on the dbgd side, where dbg-cli already solved them.
//
// Wire contract (dbg-cli protocol_version 3):
//   - every response carries `protocol_version`; mismatch is refused before
//     any payload is interpreted (the stale-plugin guard, for the two ends);
//   - non-2xx bodies are {"error": {code, message, hint}}; the code set is
//     the honest failure taxonomy (session_gone vs session_busy vs
//     command_failed vs target_running) and is preserved verbatim -- what
//     the agent does next (reconnect vs stop retrying) depends on it;
//   - exec: POST /v1/sessions/{h}/exec {command, allow_running, grace,
//     max_bytes} -> either the full result (fast commands finish within
//     `grace`) or an ack {running: true, job_id, ...} -- no request is ever
//     held open to bound a slow debugger command;
//   - jobs: GET /v1/jobs/{id}?since&max_bytes&wait -> {output, next_offset,
//     done, ...} (a long-poll read; cursors are per-reader and
//     non-destructive).

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/net/http_client.hpp"

#include "windbg_settings.hpp"

namespace minimal::tools_windbg {

// A protocol-level failure, mapped from dbgd's error body (or the transport
// layer, when the server is unreachable).
struct DbgdError {
    std::string code;     // dbgd's E_* code, or "transport" / "protocol"
    std::string message;
    std::string hint;
    int status = 0;       // HTTP status; 0 for transport failures
};

// exec's two shapes, split by the presence of `job_id` in the response.
struct ExecResult {
    // Synchronous shape (the command finished within the grace window):
    bool ok = true;
    std::string output;
    std::string error_text;
    nlohmann::json state;  // the target state AFTER the command
    // Job shape (still running / output past the budget): non-empty when the
    // result must be collected via JobRead.
    std::string job_id;
};

struct JobChunk {
    bool done = false;
    std::string output;       // the chunk AFTER `since` (possibly empty)
    std::int64_t next_offset = 0;
    bool truncated = false;
    nlohmann::json state;
};

class DbgdClient {
public:
    explicit DbgdClient(WindbgSettings settings) : settings_(std::move(settings)) {}

    const WindbgSettings& settings() const { return settings_; }

    // Opens a session against `target` (a dbgd TargetSpec object, passed
    // through verbatim -- this client is target-ignorant by design).
    // Returns the session handle.
    std::string Connect(const nlohmann::json& target, DbgdError* error);

    // Runs one command. Fast commands answer fully; slow ones answer as a
    // job (result.job_id non-empty).
    ExecResult Exec(const std::string& handle, const std::string& command,
                    bool allow_running, DbgdError* error);

    // One long-poll read of a job's transcript from `since`. `wait_s` is the
    // server-side wait; the HTTP ceiling must exceed it.
    JobChunk ReadJob(const std::string& handle, const std::string& job_id,
                    std::int64_t since, DbgdError* error);

    // The lease-free interrupt: safe to call while an exec holds the lease
    // -- that is the entire point of it.
    nlohmann::json Interrupt(const std::string& handle, DbgdError* error);

    // A query, never gated on the target being stopped.
    nlohmann::json State(const std::string& handle, DbgdError* error);

    // Teardown. `on_exit` is "detach" or "kill"; safe to call twice (the
    // second close reports session_gone, which callers treat as success).
    void Close(const std::string& handle, const std::string& on_exit);

private:
    nlohmann::json Request(const std::string& method, const std::string& path,
                           const nlohmann::json* body, DbgdError* error);
    DbgdError MapProtocol(const nlohmann::json& payload);
    std::string Query(const std::string& path,
                      std::vector<std::pair<std::string, std::string>> params) const;

    WindbgSettings settings_;
    net::HttpClient http_;
};

}  // namespace minimal::tools_windbg
