#pragma once

#include <string>

namespace minimal::tools {

// Lifecycle of a BACKGROUND task -- work a tool starts (background bash) whose
// process outlives the tool call that spawned it. The tool call itself returns
// an acknowledgment; the task reports here as it changes state, and its result
// reaches the model later via ToolContext::schedule_injection.
//
// Core-neutral on purpose: the plugin that runs the task knows nothing about
// gRPC, and the session maps this onto a wire event the same way it maps
// ToolLifecycleEvent. `tool_use_id` is the ORIGINAL invocation -- output deltas
// keep streaming under it, and a client correlates the two.
struct BackgroundTaskEvent {
    enum class State { kStarted, kCompleted, kFailed, kTimedOut, kKilled };
    std::string task_id;
    std::string tool_use_id;
    std::string command;
    int pid = 0;
    State state = State::kStarted;
};

}  // namespace minimal::tools
