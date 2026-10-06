#pragma once

// The generic background-task registry: the accounting and delivery spine
// behind "non-blocking tool call" -- the shape bash's BackgroundTaskRegistry
// carried as a bash-specific type, de-typed so any plugin can run detached
// work and report it the same way.
//
// The contract (identical to background bash's, now core-neutral):
//
//   - the tool call returns an acknowledgment immediately (task id);
//   - the task's BODY runs on its own WATCHER thread -- one per task,
//     detached, holding everything it touches by shared_ptr or by the
//     lifetime-safe handles captured from ToolContext;
//   - live output streams to the OPERATOR through emit_output, keyed to the
//     original tool_use_id, exactly as a synchronous call's would;
//   - the result reaches the MODEL as an injected message at the session's
//     next turn boundary (ToolContext::schedule_injection), never out-of-band;
//   - CANCELLATION SEMANTICS, deliberately asymmetric: cancelling the TURN
//     that started a task does not touch it (outliving the turn is the
//     mode's entire purpose, so the watcher holds no turn CancelToken);
//     session TERMINATION does -- the body polls session_lifetime and
//     self-terminates. The registry itself is process-lifetime (plugins are
//     never dlclosed).
//
// What the registry owns vs. what the body owns:
//   - owned here: id minting, the per-session record table (List /
//     CountRunning / EraseSession, with finished-record pruning), the
//     STARTED-before-watch ordering, the terminal-state-stored-LAST
//     quiescence guarantee, lifecycle-event emission, result injection, and
//     the containment wrapper (a throw out of a detached thread body calls
//     std::terminate and takes every session down -- the body is guarded,
//     with a caller-supplied cleanup hook for process-specific containment).
//   - owned by the body: the actual work -- draining a process, polling a
//     remote job, whatever -- and its own stop conditions (deadline,
//     session-death), which end in a Finish() call.

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
#include "minimal/tools/background_task_event.hpp"
#include "minimal/sidecar/policy.hpp"

namespace minimal::sidecar {

class TaskRegistry {
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

    // Shared between the registry and its watcher; the watcher holds it by
    // shared_ptr, so a session dying underneath it is a handled condition,
    // never a dangle. `pid` is advisory (0 when the task is not a process --
    // a remote job, say); it feeds events and task listings only.
    struct TaskState {
        std::string id, session_id, label;
        int pid = 0;
        std::chrono::steady_clock::time_point started_at;
        std::atomic<int> state{
            static_cast<int>(tools::BackgroundTaskEvent::State::kStarted)};
    };

    // The task's work, run on the detached watcher thread. Terminal writes
    // (event, injection, state store) go through Finish() -- never by hand --
    // so every tool's ordering matches.
    using Body = std::function<void(TaskState&, const WatchHandles&)>;

    struct StartArgs {
        std::string session_id;
        std::string label;  // the command / request, for events, logs, /tasks
        int pid = 0;
        std::string id_prefix = "bg";
        // Per-session cap on LIVE tasks. Over the limit is an error the model
        // adapts to, not a queue it blocks in -- the same ruling as
        // max_concurrent_subagents. 0 = unlimited.
        std::size_t max_running = 0;
        Body body;
        // Best-effort containment for an abnormal watcher exit and for
        // watcher-thread creation failure: kill the process group, abort the
        // remote job -- whatever releasing the task's resources means. Must
        // not throw. Called at most once, before the FAILED event.
        std::function<void()> cleanup;
        WatchHandles handles;
    };

    // Registers the task, emits STARTED, and detaches the watcher. nullopt
    // (with *error set) on refusal: the per-session limit, or a watcher
    // thread that could not be created (in which case cleanup has already
    // run and the task is marked failed).
    std::optional<std::string> Start(StartArgs args, std::string* error);

    struct TaskInfo {
        std::string id, label;
        int pid = 0;
        tools::BackgroundTaskEvent::State state = tools::BackgroundTaskEvent::State::kStarted;
        std::int64_t elapsed_ms = 0;
    };

    // Every task this session started, running ones first (they are the ones
    // a /tasks listing exists for), finished ones newest-first so the
    // operator sees what just happened without scrolling.
    std::vector<TaskInfo> List(const std::string& session_id);

    std::size_t CountRunning(const std::string& session_id);

    // Session teardown (a SessionObserver). Dropping the records is safe
    // while watchers still run: they hold their own shared_ptrs and write
    // only to those.
    void EraseSession(const std::string& session_id);

    // --- Shared helpers for bodies ------------------------------------------

    // session_lifetime is the ONLY death signal: no turn CancelToken is held,
    // because cancelling the turn that started a task must not kill the task.
    static bool SessionGone(const WatchHandles& h) { return h.session_lifetime.expired(); }

    // Lifecycle event, deliberately UNGUARDED on the normal path: a throwing
    // handle callback must escape to the containment wrapper, which degrades
    // it to a failed task (never a dead server). The registry's own failure
    // paths guard their emissions.
    static void EmitEvent(const WatchHandles& h, const TaskState& task,
                          tools::BackgroundTaskEvent::State state);

    // THE terminal sequence, in the order every tool must use: the lifecycle
    // event first (a client marks the row terminal before, or at worst
    // alongside, the injected result), then the result injection keyed to the
    // ORIGINAL tool_use_id, then -- last, after every other write -- the
    // terminal state store. That ordering makes "state is terminal" a
    // quiescence guarantee: once it reads terminal, the watcher will write
    // nothing else. kKilled means the session is gone and skips the injection
    // (it would be refused anyway).
    static void Finish(TaskState& task, const WatchHandles& handles,
                       tools::BackgroundTaskEvent::State final_state,
                       const std::string& result_text, bool is_error);

private:
    static void PruneFinishedLocked(std::vector<std::shared_ptr<TaskState>>& tasks);

    std::mutex mutex_;
    // Serializes Start()'s check-then-act: the cap check and the record push
    // span work the body will redo concurrently, and two concurrent Starts
    // could both pass the check before either pushed, overshooting
    // max_running. Held ONLY for the cap check + registration -- never
    // across the STARTED emission, the watcher-thread creation, or any other
    // plugin-seam/allocating work, which would wedge every session's Start
    // behind one slow callback.
    std::mutex start_mutex_;
    std::unordered_map<std::string, std::vector<std::shared_ptr<TaskState>>> tasks_;
    std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace minimal::sidecar
