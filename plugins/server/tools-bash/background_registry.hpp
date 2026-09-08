#pragma once

// Background bash: tasks whose process OUTLIVES the tool call that started it.
//
// The accounting-and-delivery spine (id minting, per-session records, the
// STARTED-before-watch ordering, event emission, result injection, the
// terminal-state-stored-LAST quiescence guarantee, and the containment
// wrapper around the detached watcher) is the SHARED sidecar TaskRegistry,
// which any non-blocking tool uses. What remains here is bash's own watcher
// body: drain the merged output (capped, streamed live), discard the state
// channel, reap, classify, and report.
//
// CANCELLATION SEMANTICS, deliberately asymmetric (now enforced by the shared
// registry's contract):
//   - CancelTurn does NOT touch a background task. Outliving the turn that
//     started it is the mode's entire purpose, and the watcher therefore
//     holds NO turn CancelToken.
//   - Session TERMINATION does. The watcher polls ToolContext::session_lifetime,
//     which expires with the session, and killpg + reaps on expiry. There is
//     no synchronous kill hook at teardown -- SessionObserver::OnSessionEnded
//     is dispatched fire-and-forget -- so the expiry poll (one atomic load per
//     100 ms slice) is the mechanism, and a killed task can lag termination by
//     at most one slice. The same accepted asymmetry as the killpg window.
//   - Server process exit is covered by the process-global pgid sweep in the
//     sidecar core (an atexit handler), since detached watchers die with the
//     process and their setsid'd children would otherwise be orphaned.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "minimal/sidecar/task_registry.hpp"

#include "bash_process.hpp"
#include "bash_session_state.hpp"
#include "bash_settings.hpp"

namespace minimal::tools_bash {

class BackgroundTaskRegistry {
public:
    // The lifetime-safe handles a watcher needs (sidecar's, verbatim: the
    // fields and their lifetime guarantees are the shared contract).
    using WatchHandles = minimal::sidecar::TaskRegistry::WatchHandles;

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
        minimal::tools::BackgroundTaskEvent::State state =
            minimal::tools::BackgroundTaskEvent::State::kStarted;
        std::int64_t elapsed_ms = 0;
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
    minimal::sidecar::TaskRegistry registry_;
};

}  // namespace minimal::tools_bash
