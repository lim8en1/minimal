#pragma once

// Background bash: tasks whose process OUTLIVES the tool call that started it.
//
// The shape mirrors a non-blocking subagent, one level down:
//
//   - the tool call returns an acknowledgment immediately (task id + pid);
//   - the task runs on its own WATCHER thread -- one per task, detached,
//     holding everything it touches by shared_ptr or lifetime-safe handle;
//   - the result reaches the MODEL as an injected message at the session's
//     next turn boundary (the subagent-completion mechanism, generalized:
//     ToolContext::schedule_injection), never out-of-band;
//   - live output streams to the OPERATOR through emit_output, keyed to the
//     original tool_use_id, exactly as a synchronous command's would.
//
// CANCELLATION SEMANTICS, deliberately asymmetric:
//   - CancelTurn does NOT touch a background task. Outliving the turn that
//     started it is the mode's entire purpose, and the watcher therefore
//     holds NO turn CancelToken.
//   - Session TERMINATION does. The watcher polls ToolContext::session_lifetime,
//     which expires with the session, and killpg + reaps on expiry. There is
//     no synchronous kill hook at teardown -- SessionObserver::OnSessionEnded
//     is dispatched fire-and-forget -- so the expiry poll (one atomic load per
//     100 ms slice) is the mechanism, and a killed task can lag termination by
//     at most one slice. The same accepted asymmetry as the killpg window.
//   - Server process exit is covered by the process-global pgid sweep in
//     plugin.cpp (an atexit handler), since detached watchers die with the
//     process and their setsid'd children would otherwise be orphaned.
//
// The registry itself is process-lifetime (a static inside tools-bash -- plugins
// are never dlclosed), so the detached watchers may safely hold a raw pointer
// back for nothing at all: they report completion through their captured
// handles and mark their own shared TaskState. Nothing here outlives its
// session in a way anything can observe except /tasks, which is the point.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/tools/tool.hpp"

#include "bash_process.hpp"
#include "bash_session_state.hpp"
#include "bash_settings.hpp"

namespace minimal::tools_bash {

class BackgroundTaskRegistry {
public:
    // The lifetime-safe handles a watcher needs, captured per invocation from
    // the ToolContext. All of them are safe to hold past the session's death
    // (see tools/tool.hpp): schedule_injection refuses, session_lifetime
    // expires, emit_output/emit_task_event push into a closed queue and are
    // dropped. `logger` is the process-lifetime logger.
    struct WatchHandles {
        std::function<void(std::string_view)> emit_output;
        std::function<bool(llm::Message)> schedule_injection;
        std::function<void(const tools::BackgroundTaskEvent&)> emit_task_event;
        std::weak_ptr<const void> session_lifetime;
        std::string tool_use_id;
        common::Logger* logger = nullptr;
    };

    struct StartedTask {
        std::string id;
        int pid = 0;
    };

    // Launches the process and detaches its watcher. nullopt (with *error set)
    // on refusal: the per-session task limit, or a spawn failure. Runs on the
    // session worker (it is a tool call); the fork is the same machinery the
    // synchronous path uses.
    std::optional<StartedTask> Start(const std::string& session_id, const std::string& command,
                                     const SessionShellState& state, const BashSettings& settings,
                                     std::int64_t timeout_ms, WatchHandles handles,
                                     std::string* error);

    struct TaskInfo {
        std::string id, command;
        int pid = 0;
        tools::BackgroundTaskEvent::State state = tools::BackgroundTaskEvent::State::kStarted;
        std::int64_t elapsed_ms = 0;
    };

    // Public because the watcher is a free function in the .cpp holding one of
    // these by shared_ptr; nothing outside this .so can name the type.
    struct TaskState {
        std::string id, session_id, command;
        int pid = 0;
        std::chrono::steady_clock::time_point started_at;
        std::atomic<int> state{
            static_cast<int>(tools::BackgroundTaskEvent::State::kStarted)};
    };

    // Every task this session started, running ones first (they are the ones a
    // /tasks listing exists for), finished ones retained so the operator can
    // see what happened without scrolling the transcript.
    std::vector<TaskInfo> List(const std::string& session_id);

    std::size_t CountRunning(const std::string& session_id);

    // Session teardown (BashSessionObserver::OnSessionEnded). Dropping the
    // records is safe while watchers still run: they hold their own
    // shared_ptrs and write only to those.
    void EraseSession(const std::string& session_id);

private:
    std::mutex mutex_;
    // Serializes Start()'s check-then-act: the cap check and the task push
    // span a fork(), and two concurrent Starts could both pass the check
    // before either pushed, overshooting max_background_tasks. Held only for
    // the start sequence -- never across watcher callbacks.
    std::mutex start_mutex_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<TaskState>>> tasks_;
    std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace minimal::tools_bash
