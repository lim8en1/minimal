#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "minimal/common/units.hpp"

namespace minimal::tools_python {

// Ceiling for every seconds value that feeds `now() + std::chrono::seconds(N)`
// (request timeouts, max_runtime, background_max_runtime): converting to a
// nanosecond-resolution duration overflows a 64-bit rep above ~9.2e9 seconds
// (~292 years) and wraps to a deadline in the PAST -- a request would be
// SIGINT'd and hard-killed at once despite asking for more time. Far past any
// real deadline.
inline constexpr std::int64_t kMaxSaneRuntimeSeconds = 100'000'000;  // ~3.2 years

// This plugin's config, the "config" object on its plugins.json entry. Core
// keeps it raw precisely so each plugin owns its schema -- this is that schema.
// ALL times are SECONDS (this plugin's schema contract; interpreter interaction
// is second-scale -- bash's timeout_ms stays as-is).
struct PythonSettings {
    // Fallback interpreter (used for the bare-python path and for venv
    // creation). Resolved on PATH at use time.
    std::string python = "python3";
    // Ceiling on a model-supplied sync `timeout` (seconds). 0 = unlimited. A
    // sync wait that reaches the ceiling DEMOTES to async -- it never kills
    // anything -- so the ceiling bounds the session worker without ever
    // destroying state.
    std::int64_t sync_wait_ceiling = 300;
    // Bounded spawn+handshake window for a new interpreter (a broken venv is
    // caught here, not mid-use).
    std::int64_t startup_timeout = 10;
    // Bounded venv creation / package installation.
    std::int64_t install_timeout = 300;
    // How long python_interrupt waits for evidence the SIGINT landed before
    // reporting "no reaction".
    std::int64_t grace_seconds = 2;
    // Bounds the PERSISTED result text per request (the transcript's copy).
    std::int64_t max_output_bytes = common::kMiB;
    // Driver-side per-stream capture cap (stdout/stderr inside the
    // interpreter), communicated via _MINIMAL_PY_CAPTURE at spawn. Bounds the
    // protocol line size; max_output_bytes bounds what is persisted.
    std::int64_t max_capture_bytes = common::kMiB;
    // RLIMIT_CPU backstop for the interpreter process, seconds. 0 = disabled:
    // the interpreter's clock is the session's clock, and a CPU-spinning live
    // session is the model's/user's to interrupt -- the default stance is
    // "timeouts demote, never destroy".
    std::int64_t rlimit_cpu_seconds = 0;
    // RLIMIT_FSIZE backstop for files the executed code writes.
    std::int64_t max_file_size_bytes = common::kGiB;
    // The live-output ring for python_output polling (head+tail windows over
    // the running request's stdout stream; see stream_ring.hpp). The ring
    // exists only while a request is running and is discarded on completion.
    std::int64_t stream_head_bytes = 256 * common::kKiB;
    std::int64_t stream_tail_bytes = 768 * common::kKiB;
    // Default deadline for background requests (seconds; 0 = unlimited, the
    // stance "timeouts demote, never destroy"). A per-call python_exec
    // max_runtime overrides it; on expiry the reader soft-interrupts and --
    // if the request ignores SIGINT for grace_seconds -- hard-kills the
    // interpreter (state loss, the caller's explicit deadline). This is the
    // ONE destructive timer in the plugin, and it is opt-in.
    std::int64_t background_max_runtime = 0;
};

// Every field optional; a missing field keeps the struct's default. A present
// field of the wrong type or out of range, or an unknown field, throws
// config::ConfigError naming it -- the plugin then fails to load, loudly.
PythonSettings ParsePythonSettings(const nlohmann::json& config);

// The preset's `tool_config.python` slice -- the per-preset DECLARED package
// contract. Merged per-key over the plugin config would conflate two different
// things: the slice carries the DECLARED SET (packages/requirements) and the
// plugin config carries operator knobs (timeouts, binaries). They do not share
// keys, so there is nothing to merge -- the slice is read per invocation from
// ToolContext::tool_config, the knobs are parsed once at registration.
struct PythonSlice {
    // Package specs, verbatim ("impacket", "pefile==2023.2.7").
    std::vector<std::string> packages;
    // Requirements files, project_root-relative paths.
    std::vector<std::string> requirements;

    bool empty() const { return packages.empty() && requirements.empty(); }
};

// Null-safe: a preset that declares no slice yields an empty contract.
PythonSlice ParseSlice(const nlohmann::json* tool_config);

}  // namespace minimal::tools_python
