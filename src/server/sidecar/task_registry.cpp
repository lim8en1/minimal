#include "minimal/sidecar/task_registry.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace minimal::sidecar {

namespace {

using tools::BackgroundTaskEvent;

bool StateIsRunning(BackgroundTaskEvent::State state) {
    return state == BackgroundTaskEvent::State::kStarted;
}

}  // namespace

void TaskRegistry::EmitEvent(const WatchHandles& h, const TaskState& task,
                             BackgroundTaskEvent::State state) {
    if (!h.emit_task_event) return;
    // Deliberately UNGUARDED on the normal path: a throwing handle callback
    // must escape to the containment wrapper (or to the Start caller), which
    // degrades it to a failed task -- the same ruling background bash always
    // made. The FAILURE paths guard their own emissions, below and in Start,
    // because an exception there would escape a catch or a thread body.
    BackgroundTaskEvent event;
    event.task_id = task.id;
    event.tool_use_id = h.tool_use_id;
    event.command = task.label;
    event.pid = task.pid;
    event.state = state;
    h.emit_task_event(event);
}

void TaskRegistry::Finish(TaskState& task, const WatchHandles& handles,
                          BackgroundTaskEvent::State final_state,
                          const std::string& result_text, bool is_error) {
    // Lifecycle event first, so a client marks the row terminal before (or at
    // worst alongside) the injected result arrives. A throw out of the sink
    // propagates to the body's containment wrapper, which fails the task --
    // the terminal state is then stored by the containment path.
    EmitEvent(handles, task, final_state);

    // kKilled means the session is gone; do not bother staging a message its
    // session will refuse (Push would return false -- skipping is equivalent,
    // just quieter).
    if (final_state != BackgroundTaskEvent::State::kKilled && handles.schedule_injection) {
        llm::ToolResultBlock block;
        block.tool_use_id = handles.tool_use_id;
        block.is_error = is_error;
        block.content.push_back(llm::TextBlock{result_text});
        if (!handles.schedule_injection(llm::Message{llm::Role::User, {std::move(block)}})) {
            if (handles.logger) {
                handles.logger->warn("{}[{}] task {}: result dropped; session already terminating",
                                     "task", task.session_id, task.id);
            }
        }
    }

    // THE terminal state is stored LAST -- after the lifecycle event and the
    // injection, never before. That makes "state is terminal" a quiescence
    // guarantee for every waiter on it (CountRunning, the tests' joins): once
    // it reads terminal, this thread will write nothing else.
    task.state.store(static_cast<int>(final_state), std::memory_order_release);
}

void TaskRegistry::PruneFinishedLocked(std::vector<std::shared_ptr<TaskState>>& tasks) {
    std::size_t finished = 0;
    for (const auto& task : tasks) {
        if (!StateIsRunning(
                static_cast<BackgroundTaskEvent::State>(task->state.load(std::memory_order_acquire)))) {
            ++finished;
        }
    }
    std::size_t excess = finished > kMaxFinishedRecordsPerSession
                             ? finished - kMaxFinishedRecordsPerSession
                             : 0;
    if (excess == 0) return;
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [&excess](const std::shared_ptr<TaskState>& task) {
                                   if (excess == 0 ||
                                       StateIsRunning(static_cast<BackgroundTaskEvent::State>(
                                           task->state.load(std::memory_order_acquire)))) {
                                       return false;
                                   }
                                   --excess;
                                   return true;
                               }),
                tasks.end());
}

std::optional<std::string> TaskRegistry::Start(StartArgs args, std::string* error) {
    // Over the limit is an ERROR THE MODEL ADAPTS TO, not a queue it blocks
    // in. start_mutex_ closes the check-then-act gap: the check and the push
    // below span the watcher's creation, and two concurrent Starts could both
    // pass the check before either pushed. (A task FINISHING between the two
    // only lowers the count -- the safe direction -- so this lock suffices.)
    std::lock_guard<std::mutex> start_lock(start_mutex_);
    if (args.max_running > 0 && CountRunning(args.session_id) >= args.max_running) {
        *error = "background task limit reached (" + std::to_string(args.max_running) +
                 " running); wait for one to finish";
        return std::nullopt;
    }

    auto task = std::make_shared<TaskState>();
    task->id = args.id_prefix + "-" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
    task->session_id = args.session_id;
    task->label = args.label;
    task->pid = args.pid;
    task->started_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[args.session_id].push_back(task);
        PruneFinishedLocked(tasks_[args.session_id]);
    }

    // STARTED goes out before any output delta can (the watcher does not
    // exist yet), so a client never sees output for a task it was not told
    // about.
    EmitEvent(args.handles, *task, BackgroundTaskEvent::State::kStarted);

    // The containment wrapper. Every plugin seam runs under a guard because
    // plugin code is arbitrary; a detached thread body is the one place where
    // an uncaught exception does not merely fail a call -- it calls
    // std::terminate and takes every session in the process down. The body's
    // own failure paths should degrade to task-failed values; this catches
    // what those paths cannot (a throw out of a handle callback, out of the
    // allocator itself). Best-effort throughout: the first exception already
    // means something is broken, and a second must not escape.
    auto body = std::move(args.body);
    auto cleanup = std::move(args.cleanup);
    auto guarded = [task, body = std::move(body), cleanup = std::move(cleanup),
                    handles = args.handles]() mutable {
        try {
            body(*task, handles);
        } catch (const std::exception& e) {
            if (cleanup) {
                try {
                    cleanup();
                } catch (...) {
                }
            }
            if (handles.logger) {
                handles.logger->error("{}[{}] task {}: watcher failed: {}", "task",
                                      task->session_id, task->id, e.what());
            }
            // Failure path: the emission is guarded -- an exception here
            // would escape a catch that is already cleaning up after a throw.
            try {
                EmitEvent(handles, *task, BackgroundTaskEvent::State::kFailed);
            } catch (...) {
            }
            task->state.store(static_cast<int>(BackgroundTaskEvent::State::kFailed),
                              std::memory_order_release);
        } catch (...) {
            if (cleanup) {
                try {
                    cleanup();
                } catch (...) {
                }
            }
            if (handles.logger) {
                handles.logger->error("{}[{}] task {}: watcher failed (unknown exception)",
                                      "task", task->session_id, task->id);
            }
            try {
                EmitEvent(handles, *task, BackgroundTaskEvent::State::kFailed);
            } catch (...) {
            }
            task->state.store(static_cast<int>(BackgroundTaskEvent::State::kFailed),
                              std::memory_order_release);
        }
    };

    // Detached, never joined: the task may outlive the turn, the worker, and
    // the session. Everything the thread touches is owned -- the shared
    // TaskState, the handles (channels-backed), the body's captures -- so no
    // teardown path can strand it on freed memory. The registry itself is a
    // process-lifetime object inside the owning plugin (or core).
    //
    // std::thread's constructor allocates and can throw (resource exhaustion)
    // -- and by this point the task is already registered and announced as
    // STARTED. Contained here rather than in the watcher's guard, because the
    // watcher thread is precisely what failed to come into existence: run the
    // cleanup, mark the task terminal so CountRunning never wedges the
    // session at its limit on a task that will never report.
    try {
        std::thread(std::move(guarded)).detach();
    } catch (...) {
        if (cleanup) {
            try {
                cleanup();
            } catch (...) {
            }
        }
        task->state.store(static_cast<int>(BackgroundTaskEvent::State::kFailed),
                          std::memory_order_release);
        try {
            EmitEvent(args.handles, *task, BackgroundTaskEvent::State::kFailed);
        } catch (...) {
        }
        common::Logger& logger =
            args.handles.logger ? *args.handles.logger : common::DefaultLogger();
        logger.error("{}[{}] task {}: watcher thread could not start; task killed", "task",
                     task->session_id, task->id);
        *error = "failed to start background task (could not create watcher thread)";
        return std::nullopt;
    }

    return task->id;
}

std::vector<TaskRegistry::TaskInfo> TaskRegistry::List(const std::string& session_id) {
    std::vector<TaskInfo> running, finished;
    std::vector<std::shared_ptr<TaskState>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = tasks_.find(session_id); it != tasks_.end()) snapshot = it->second;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& task : snapshot) {
        TaskInfo info;
        info.id = task->id;
        info.label = task->label;
        info.pid = task->pid;
        info.state = static_cast<BackgroundTaskEvent::State>(
            task->state.load(std::memory_order_acquire));
        info.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - task->started_at)
                              .count();
        if (StateIsRunning(info.state)) {
            running.push_back(std::move(info));
        } else {
            finished.push_back(std::move(info));
        }
    }
    // Finished tasks, newest first: the ones that just completed are the ones
    // the operator is looking for. Running keeps registry order (oldest first
    // -- the one that has been running longest is the one to check on).
    std::reverse(finished.begin(), finished.end());
    running.insert(running.end(), finished.begin(), finished.end());
    return running;
}

std::size_t TaskRegistry::CountRunning(const std::string& session_id) {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = tasks_.find(session_id); it != tasks_.end()) {
        for (const auto& task : it->second) {
            const auto state = static_cast<BackgroundTaskEvent::State>(
                task->state.load(std::memory_order_acquire));
            if (StateIsRunning(state)) ++count;
        }
    }
    return count;
}

void TaskRegistry::EraseSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(session_id);
}

}  // namespace minimal::sidecar
