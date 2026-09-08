#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::tools_windbg {

// This plugin's config, the "config" object on its plugins.json entry. Core
// keeps it raw precisely so each plugin owns its schema -- this is that
// schema. All times are SECONDS unless suffixed _ms.
//
// The tool is a CLIENT of a dbgd server (the debugger engine lives there --
// see the dbg-cli project): one dbgd owns one engine (WinDbg/DbgEng via
// pybag), this plugin talks to it over HTTPS/JSON and maps it onto the
// harness's tool + non-blocking-task machinery.
struct WindbgSettings {
    // Base URL of the dbgd server, e.g. "http://debugger-host:8477". Must
    // match the port dbgd was configured with (`dbgd configure` /
    // server.json). Plain HTTP is accepted for loopback use; put dbgd behind
    // TLS for anything else (see dbg-cli's `dbgd configure`).
    std::string base_url = "http://127.0.0.1:8477";
    // Bearer token. dbgd refuses unauthenticated requests; set the token
    // `dbgd configure` generated. Empty = send no auth header (a local
    // dbgd started with --insecure-auth would accept it).
    std::string bearer_token;
    // Engine backend the server must serve (dbgd refuses mismatches).
    std::string backend = "windbg";
    // Wire protocol version, checked against every response's
    // protocol_version field -- the "you forgot to upgrade one side" guard.
    int protocol_version = 3;
    // Passed to exec as `grace` (seconds): how long dbgd tries to finish a
    // command before acknowledging it as a job. Fast commands land inside
    // the grace and answer synchronously; slow ones come back as jobs.
    std::int64_t exec_grace_s = 2;
    // HTTP request ceiling for connect/state/interrupt/job polls (ms). A job
    // poll waits at most `job_wait_s` server-side; the HTTP ceiling must be
    // strictly larger or the poll would be cut off client-side.
    std::int64_t request_timeout_ms = 30000;
    // Server-side long-poll per job read (seconds).
    std::int64_t job_wait_s = 5;
    // Sync-wait semantics, mirroring python_exec: `timeout` input 0 = run
    // async (ack + injected result); >0 = wait up to N seconds on the
    // worker, then DEMOTE to async -- never kill anything. The ceiling
    // bounds a session worker; it does not destroy debugger state.
    std::int64_t sync_wait_ceiling = 300;
    // Bounds the PERSISTED result text per command.
    std::int64_t max_output_bytes = 256 * 1024;
    // Per-session cap on live background debugger jobs (the TaskRegistry
    // limit; over the limit is an error the model adapts to).
    int max_jobs = 4;
    // TargetSpec used when a call supplies no `target` and no session exists
    // yet (dbgd POST /v1/sessions `target` object): e.g.
    // {"kind": "exec", "value": "C:\\path\\app.exe"} or
    // {"kind": "pid", "value": 4242} or {"kind": "kernel",
    // "connection": "net:port=50000,key=..."} or {"kind": "exec",
    // "server": "tcp:server=10.0.0.9,port=5555", ...} to reach a dbgsrv
    // process server on the target machine. Empty = require the `target`
    // input on first connect.
    nlohmann::json default_target;
    // Passed as `on_exit` when a session is closed because the harness
    // session ended: "detach" (default; the target keeps running) or "kill".
    std::string on_session_end = "detach";
};

// Every field optional; missing values keep the struct's defaults, so an
// absent or partial config object is normal rather than an error.
WindbgSettings ParseWindbgSettings(const nlohmann::json& config);

}  // namespace minimal::tools_windbg
