#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include "minimal/common/units.hpp"
#include "minimal/sidecar/policy.hpp"
#include "policy.hpp"

namespace minimal::tools_bash {

// This plugin's own config, the "config" object on its plugins.json entry.
// Core keeps it as raw JSON precisely so each plugin owns its own schema --
// this is that schema, and nothing outside tools-bash needs to know it.
struct BashSettings {
    // Wall-clock cap per invocation; the whole process group is killed at the
    // deadline. The RLIMIT_CPU backstop derives from this (see bash_tool.cpp).
    // Configured in SECONDS (`timeout_s`) -- the unit the model-facing tool
    // input uses too -- and stored in ms, the unit the process plumbing
    // (deadlines, RLIMIT_CPU, logs) already speaks.
    std::int64_t timeout_ms = kDefaultTimeout.count();
    // Ceiling on the model-supplied `timeout` (seconds). The model may ask for
    // longer than the default (a build is not `ls`) but not without bound: the
    // deadline is the only hard limit on a session worker thread.
    std::int64_t max_timeout_ms = kDefaultMaxTimeout.count();
    // Background tasks have their OWN ceiling, and it exists for a different
    // reason: a background task never holds the session worker, so the sync
    // rationale ("the deadline is the only hard bound on a session worker
    // thread") does not apply. What applies is that nothing may run forever on
    // the operator's machine -- bounded, but separately, and generous. A model
    // that supplies no timeout gets the ceiling itself: a long-running build is
    // the entire point of the mode. Still killable at any time (kill -- -PID
    // from a later call) and killed when the session ends.
    std::int64_t background_max_timeout_ms = kDefaultBackgroundMaxTimeout.count();
    // Per-session cap on LIVE background tasks. Over the limit is an error
    // result the model adapts to, not a queue -- the same ruling as
    // max_concurrent_subagents.
    int max_background_tasks = kDefaultMaxBackgroundTasks;
    // Bounds the PERSISTED result (the ToolResultBlock the model sees). Live
    // streaming through emit_output is uncapped -- that channel is lossy by
    // design, this one is the record.
    std::int64_t max_output_bytes = kDefaultMaxOutputBytes;
    // RLIMIT_FSIZE backstop, reachable now that redirection works (`cmd > big`).
    std::int64_t max_file_size_bytes = sidecar::kFileSizeBackstopBytes;
    // MemoryMax of the transient systemd scope each command runs in, percent
    // of physical RAM; 0 = no scope. Linux with a reachable user manager only
    // (see memory_scope.hpp); elsewhere commands run unscoped regardless.
    int memory_max_percent = kDefaultMemoryMaxPercent;
    // Operator-swappable, like everything else here: a different POSIX shell, or
    // a wrapper that enters a container, needs no code change.
    std::string shell = "/bin/sh";
    // "allowlist" (default): the session's shell environment seeds from a
    // filtered set ($HOME, $PATH, $TMPDIR, ... -- see bash_session_state.cpp).
    // The server process's environment carries provider API keys; seeding ALL
    // of it let a model-authored `env` persist credentials into the
    // transcript. "all" restores the full seed as an explicit operator choice.
    std::string inherit_env = "allowlist";
};

// Every field optional; a missing field keeps the struct's default. A present
// field of the wrong type or out of range, or an unknown field, throws
// config::ConfigError naming it -- the plugin then fails to load, loudly.
BashSettings ParseBashSettings(const nlohmann::json& config);

}  // namespace minimal::tools_bash
