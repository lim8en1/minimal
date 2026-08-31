#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::tools_bash {

// This plugin's own config, the "config" object on its plugins.json entry.
// Core keeps it as raw JSON precisely so each plugin owns its own schema --
// this is that schema, and nothing outside tools-bash needs to know it.
struct BashSettings {
    // Wall-clock cap per invocation; the whole process group is killed at the
    // deadline. The RLIMIT_CPU backstop derives from this (see bash_tool.cpp).
    std::int64_t timeout_ms = 120000;
    // Ceiling on the model-supplied `timeout_ms`. The model may ask for longer
    // than the default (a build is not `ls`) but not without bound: the deadline
    // is the only hard limit on a session worker thread.
    std::int64_t max_timeout_ms = 600000;
    // Background tasks have their OWN ceiling, and it exists for a different
    // reason: a background task never holds the session worker, so the sync
    // rationale ("the deadline is the only hard bound on a session worker
    // thread") does not apply. What applies is that nothing may run forever on
    // the operator's machine -- bounded, but separately, and generous. A model
    // that supplies no timeout_ms gets the ceiling itself: a long-running build
    // is the entire point of the mode. Still killable at any time (kill -- -PID
    // from a later call) and killed when the session ends.
    std::int64_t background_max_timeout_ms = 600000;
    // Per-session cap on LIVE background tasks. Over the limit is an error
    // result the model adapts to, not a queue -- the same ruling as
    // max_concurrent_subagents.
    int max_background_tasks = 4;
    // Bounds the PERSISTED result (the ToolResultBlock the model sees). Live
    // streaming through emit_output is uncapped -- that channel is lossy by
    // design, this one is the record.
    std::int64_t max_output_bytes = 1024 * 1024;
    // RLIMIT_FSIZE backstop, reachable now that redirection works (`cmd > big`).
    std::int64_t max_file_size_bytes = 1024LL * 1024 * 1024;
    // Operator-swappable, like everything else here: a different POSIX shell, or
    // a wrapper that enters a container, needs no code change.
    std::string shell = "/bin/sh";
};

// Every field optional; missing values keep the struct's defaults, so an absent
// or partial config object is normal rather than an error.
BashSettings ParseBashSettings(const nlohmann::json& config);

}  // namespace minimal::tools_bash
