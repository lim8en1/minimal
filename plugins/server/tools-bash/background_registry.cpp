#include "background_registry.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <thread>
#include <utility>

namespace minimal::tools_bash {
namespace {

using tools::BackgroundTaskEvent;

// The watcher's drain slice. Long enough that a background task costs almost
// nothing while it streams nothing; short enough that session-death expiry (a
// 100 ms granularity decision) is felt within a fraction of a second.
constexpr int kPollSliceMs = 100;
// The reap slice after EOF: the sync path uses 10 ms because the worker is
// blocked; the watcher is not, and a slower slice changes nothing observable.
constexpr int kReapSliceMs = 25;

// Kills the group, then reaps with a BOUND. "Bounded by SIGKILL" (the old
// blocking waitpid's justification) does not hold for a process stuck in
// uninterruptible disk sleep (D state): SIGKILL stays pending indefinitely
// and the blocking call would park the watcher thread forever. Poll WNOHANG
// with a generous ceiling instead; past it, `reaped` stays false and the
// caller reports the status as undetermined rather than hanging.
constexpr int kPostKillReapMs = 5000;
int ReapAfterKill(pid_t pid, bool& reaped) {
    KillProcessGroup(pid);
    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPostKillReapMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            reaped = true;
            break;
        }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;  // ECHILD: nothing to reap; caller flags it
        poll(nullptr, 0, kReapSliceMs);
    }
    return status;
}

bool StateIsRunning(BackgroundTaskEvent::State state) {
    return state == BackgroundTaskEvent::State::kStarted;
}

// Finished records are retained for /tasks visibility, but unboundedly: a
// long-lived session that runs thousands of background tasks kept every record
// forever (the only removal was whole-session teardown). Cap them, dropping
// the OLDEST finished records first; running records are never touched.
constexpr std::size_t kMaxFinishedRecordsPerSession = 32;

bool StateIsRunningLocked(const std::shared_ptr<BackgroundTaskRegistry::TaskState>& task) {
    return StateIsRunning(
        static_cast<BackgroundTaskEvent::State>(task->state.load(std::memory_order_acquire)));
}

void PruneFinishedLocked(std::vector<std::shared_ptr<BackgroundTaskRegistry::TaskState>>& tasks) {
    std::size_t finished = 0;
    for (const auto& task : tasks) {
        if (!StateIsRunningLocked(task)) ++finished;
    }
    std::size_t excess = finished > kMaxFinishedRecordsPerSession
                             ? finished - kMaxFinishedRecordsPerSession
                             : 0;
    if (excess == 0) return;
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [&excess](const std::shared_ptr<BackgroundTaskRegistry::TaskState>& task) {
                                   if (excess == 0 || StateIsRunningLocked(task)) return false;
                                   --excess;
                                   return true;
                               }),
                tasks.end());
}

// The watcher. Detached; owns its state through shared_ptr/owned copies, so a
// session dying underneath it is a handled condition, never a dangle.
//
// Lifecycle: drain stdout+stderr (capped; streamed live) and DISCARD the state
// channel (a background task's final cwd/env is stale by the time it reports;
// adopting it would stomp what foreground calls set up in between -- but the
// channel must still be drained, or a child blocked writing a full pipe never
// reaches its own exit), reap deadline-bounded, classify, emit the lifecycle
// event, inject the result.
void WatchTaskImpl(std::shared_ptr<BackgroundTaskRegistry::TaskState> task,
                   LaunchedProcess proc, BashSettings settings, std::int64_t timeout_ms,
                   BackgroundTaskRegistry::WatchHandles handles) {
    ScopedFd out_read(proc.out_fd), state_read(proc.state_fd);
    common::Logger& logger =
        handles.logger ? *handles.logger : common::DefaultLogger();

    CappedBuffer buffer(settings.max_output_bytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
    // session_lifetime is the ONLY death signal: no turn CancelToken is held,
    // because cancelling the turn that started a task must not kill the task.
    const auto session_gone = [&] { return handles.session_lifetime.expired(); };

    bool out_open = true, state_open = true;
    auto exit = BackgroundTaskEvent::State::kStarted;  // kStarted == still running
    bool io_error = false;
    bool reap_error = false;

    while ((out_open || state_open) && exit == BackgroundTaskEvent::State::kStarted) {
        if (session_gone()) {
            exit = BackgroundTaskEvent::State::kKilled;
            break;
        }
        if (expired()) {
            exit = BackgroundTaskEvent::State::kTimedOut;
            break;
        }

        pollfd fds[2];
        int nfds = 0;
        const int out_slot = out_open ? nfds++ : -1;
        if (out_open) fds[out_slot] = pollfd{out_read.get(), POLLIN, 0};
        const int state_slot = state_open ? nfds++ : -1;
        if (state_open) fds[state_slot] = pollfd{state_read.get(), POLLIN, 0};

        const int rc = poll(fds, static_cast<nfds_t>(nfds), kPollSliceMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            // Same treatment as the sync path, minus the retry: a watcher
            // losing its pipe is a degraded task, not a crashed session.
            io_error = true;
            break;
        }
        if (rc == 0) continue;

        char chunk[8192];
        if (out_slot >= 0 && (fds[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(out_read.get(), chunk, sizeof(chunk));
            if (n > 0) {
                const std::string_view view(chunk, static_cast<std::size_t>(n));
                buffer.Append(view);
                // Live channel to the operator, keyed to the original
                // tool_use_id; lossy by design, capped record lives here.
                if (handles.emit_output) handles.emit_output(view);
            } else if (n == 0 || errno != EINTR) {
                out_open = false;
            }
        }
        if (state_slot >= 0 && (fds[state_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            // Read and discard: see the state-channel note above. The 1 MiB
            // payload cap is irrelevant here because nothing is retained.
            const ssize_t n = read(state_read.get(), chunk, sizeof(chunk));
            if (n == 0 || (n < 0 && errno != EINTR)) state_open = false;
        }
    }

    // Reaping. For a natural exit this is deadline-bounded WNOHANG slices --
    // EOF on the pipes does not mean the process is gone (it can close its
    // stdio and keep running) -- with the same expiry/deadline checks the
    // drain loop had. For a killed/timed-out task the kill comes first and
    // the blocking reap is bounded by SIGKILL.
    int wait_status = 0;
    bool reaped = false;
    if (exit == BackgroundTaskEvent::State::kStarted && !io_error) {
        while (true) {
            const pid_t rc = waitpid(proc.pid, &wait_status, WNOHANG);
            if (rc == proc.pid) {
                reaped = true;
                break;
            }
            if (rc < 0) {
                if (errno == EINTR) continue;
                // ECHILD: the pid was reaped out from under us, so
                // wait_status is meaningless -- leaving it at 0 would
                // classify this task as a clean exit below. Flagged instead.
                reap_error = true;
                break;
            }
            if (session_gone()) {
                exit = BackgroundTaskEvent::State::kKilled;
                break;
            }
            if (expired()) {
                exit = BackgroundTaskEvent::State::kTimedOut;
                break;
            }
            poll(nullptr, 0, kReapSliceMs);
        }
    }
    // An ECHILD reap is the one not-killed path where the group must not be
    // killed either: the pid is already gone, and the status is unknowable.
    bool killed_and_reaped = false;
    if (!reaped && !reap_error) wait_status = ReapAfterKill(proc.pid, killed_and_reaped);
    if (!reaped && !reap_error && !killed_and_reaped) {
        // The kill did not land within the post-kill reap ceiling (D state).
        // wait_status is meaningless; classify as failed rather than reading
        // 0 as a clean exit -- and the watcher still ends, bounded.
        reap_error = true;
    }
    // Reaped (or killed and reaped): the pgid is dead and leaves the sweep set.
    ForgetPgid(proc.pid);

    if (exit == BackgroundTaskEvent::State::kStarted) {
        if (io_error) {
            exit = BackgroundTaskEvent::State::kFailed;
        } else if (reap_error) {
            exit = BackgroundTaskEvent::State::kFailed;
        } else if (WIFSIGNALED(wait_status)) {
            exit = BackgroundTaskEvent::State::kFailed;
        } else {
            const int code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
            exit = code == 0 ? BackgroundTaskEvent::State::kCompleted
                             : BackgroundTaskEvent::State::kFailed;
        }
    }

    // THE terminal state is stored LAST -- after the lifecycle event and the
    // injection, never before. That makes "state is terminal" a quiescence
    // guarantee for every waiter on it (CountRunning, TaskState, the tests'
    // joins): once it reads terminal, this thread will write nothing else. The
    // state rides the event explicitly, so nothing reads the field to learn
    // the outcome, and /tasks reporting a still-running task for the instants
    // before the store is the harmless direction.
    const auto StoreTerminalState = [&] {
        task->state.store(static_cast<int>(exit), std::memory_order_release);
    };
    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - task->started_at)
            .count();
    switch (exit) {
        case BackgroundTaskEvent::State::kCompleted:
            logger.info("bash[{}] background {} ok: '{}' ({} ms)", task->session_id, task->id,
                        task->command, duration_ms);
            break;
        case BackgroundTaskEvent::State::kKilled:
            logger.warn("bash[{}] background {}: session ended; '{}' killed ({} ms)",
                        task->session_id, task->id, task->command, duration_ms);
            break;
        default:
            logger.warn("bash[{}] background {} ended (state {}): '{}' ({} ms)",
                        task->session_id, task->id, static_cast<int>(exit), task->command,
                        duration_ms);
            break;
    }

    // Lifecycle event first, so a client marks the row terminal before (or at
    // worst alongside) the injected result arrives.
    if (handles.emit_task_event) {
        BackgroundTaskEvent event;
        event.task_id = task->id;
        event.tool_use_id = handles.tool_use_id;
        event.command = task->command;
        event.pid = task->pid;
        event.state = exit;
        handles.emit_task_event(event);
    }

    // kKilled means the session is gone; do not bother staging a message its
    // session will refuse (Push would return false -- skipping is equivalent,
    // just quieter).
    if (exit == BackgroundTaskEvent::State::kKilled) {
        StoreTerminalState();
        return;
    }

    const std::string output = buffer.Render();
    const auto with_output = [&](const std::string& footer) {
        return output.empty() ? footer : output + "\n" + footer;
    };
    std::string text;
    bool is_error = false;
    switch (exit) {
        case BackgroundTaskEvent::State::kCompleted:
            text = with_output("[background task " + task->id + " completed]");
            break;
        case BackgroundTaskEvent::State::kFailed: {
            is_error = true;
            if (reap_error) {
                text = "[could not determine exit status (process already reaped)]";
            } else if (WIFSIGNALED(wait_status)) {
                text = with_output("[terminated by signal " +
                                   std::to_string(WTERMSIG(wait_status)) + "]");
            } else if (io_error) {
                text = "[background task " + task->id + " failed reading output]";
            } else {
                const int code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
                text = with_output("[exit status: " + std::to_string(code) + "]");
            }
            break;
        }
        case BackgroundTaskEvent::State::kTimedOut:
            is_error = true;
            text = with_output("[timed out after " + std::to_string(timeout_ms) +
                               " ms; process group killed]");
            break;
        default:
            break;  // kStarted/kKilled: unreachable here
    }

    // The result reaches the model as a tool-result user message keyed to the
    // ORIGINAL invocation -- the exact shape a non-blocking subagent's
    // completion uses, and the session-naming rule already knows not to title
    // a tool-result-only user message. False = session terminating; log it.
    llm::ToolResultBlock block;
    block.tool_use_id = handles.tool_use_id;
    block.is_error = is_error;
    block.content.push_back(llm::TextBlock{std::move(text)});
    if (!handles.schedule_injection(llm::Message{llm::Role::User, {std::move(block)}})) {
        logger.warn("bash[{}] background {}: result dropped; session already terminating",
                    task->session_id, task->id);
    }

    StoreTerminalState();
}

// Terminal state after a containment catch. Every step guarded: the handle
// callbacks are the likeliest throwers, so a failure inside the failure path
// must degrade to a log line, never escape the detached thread. The event is
// emitted BEFORE the state is stored -- the same "lifecycle event first"
// ordering the normal path uses -- so a waiter that observes the terminal
// state knows every write the thread will ever make has completed.
void MarkFailedQuietly(const std::shared_ptr<BackgroundTaskRegistry::TaskState>& task,
                       const BackgroundTaskRegistry::WatchHandles& handles) {
    if (handles.emit_task_event) {
        try {
            BackgroundTaskEvent event;
            event.task_id = task->id;
            event.tool_use_id = handles.tool_use_id;
            event.command = task->command;
            event.pid = task->pid;
            event.state = BackgroundTaskEvent::State::kFailed;
            handles.emit_task_event(event);
        } catch (...) {
        }
    }
    task->state.store(static_cast<int>(BackgroundTaskEvent::State::kFailed),
                      std::memory_order_release);
}

// Containment for the containment wrapper: the normal exit of WatchTaskImpl
// always kills-or-reaps and calls ForgetPgid (leaving nothing live in the
// atexit sweep set), but a throw out of the body skips all of that -- a live
// process group plus a stale pgid registration, which pid reuse turns into a
// wrong-target kill at process exit. Best-effort throughout: the first
// exception already means something is broken, and a second must not escape a
// detached thread. Both halves are idempotent, so an exception AFTER the body
// reaped normally (e.g. out of the result injection) lands here as no-ops.
void ContainOrphanedTask(pid_t pid) {
    try {
        bool reaped = false;
        ReapAfterKill(pid, reaped);  // SIGKILLs the group, reaps bounded
    } catch (...) {
    }
    try {
        ForgetPgid(pid);
    } catch (...) {
    }
}

// The containment wrapper. Every other plugin seam in the tree runs under a
// guard because plugin code is arbitrary; this thread was the one exception,
// and it is the one that must OUTLIVE its invocation -- an exception escaping
// a thread function calls std::terminate and takes every session in the
// process down. The body's own failure paths already degrade to task-failed
// values; this catches what those paths cannot (a throw out of a handle
// callback, out of buffer rendering, out of the allocator itself).
void WatchTask(std::shared_ptr<BackgroundTaskRegistry::TaskState> task,
               LaunchedProcess proc, BashSettings settings, std::int64_t timeout_ms,
               BackgroundTaskRegistry::WatchHandles handles) {
    try {
        WatchTaskImpl(task, std::move(proc), settings, timeout_ms, handles);
    } catch (const std::exception& e) {
        common::Logger& logger =
            handles.logger ? *handles.logger : common::DefaultLogger();
        // pid_t survives the move (only the ScopedFds are drained), so the
        // group is still reachable here.
        ContainOrphanedTask(proc.pid);
        logger.error("bash[{}] background {}: watcher failed: {}", task->session_id, task->id,
                     e.what());
        MarkFailedQuietly(task, handles);
    } catch (...) {
        common::Logger& logger =
            handles.logger ? *handles.logger : common::DefaultLogger();
        ContainOrphanedTask(proc.pid);
        logger.error("bash[{}] background {}: watcher failed (unknown exception)",
                     task->session_id, task->id);
        MarkFailedQuietly(task, handles);
    }
}

}  // namespace

std::optional<BackgroundTaskRegistry::StartedTask> BackgroundTaskRegistry::Start(
    const std::string& session_id, const std::string& command, const SessionShellState& state,
    const BashSettings& settings, std::int64_t timeout_ms, WatchHandles handles,
    std::string* error) {
    // Over the limit is an ERROR THE MODEL ADAPTS TO, not a queue it blocks
    // in -- the same ruling as max_concurrent_subagents. A tool that hangs is
    // a turn that never ends.
    //
    // start_mutex_ closes the check-then-act gap: the check and the push
    // below span a fork(), and two concurrent Starts could both pass the
    // check before either pushed. (A task FINISHING between the two only
    // lowers the count -- the safe direction -- so this lock is sufficient.)
    std::lock_guard<std::mutex> start_lock(start_mutex_);
    const std::size_t running = CountRunning(session_id);
    if (running >= static_cast<std::size_t>(settings.max_background_tasks)) {
        *error = "background task limit reached (" + std::to_string(settings.max_background_tasks) +
                 " running); wait for one to finish or kill it with kill -- -PID";
        return std::nullopt;
    }

    const auto wrapped = WrapCommand(command);
    LaunchedProcess proc = LaunchProcess(wrapped, state, settings, timeout_ms);
    if (!proc.ok) {
        *error = "failed to start shell: " + proc.spawn_error;
        return std::nullopt;
    }

    auto task = std::make_shared<TaskState>();
    task->id = "bg-" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
    task->session_id = session_id;
    task->command = command;
    task->pid = proc.pid;
    task->started_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[session_id].push_back(task);
        PruneFinishedLocked(tasks_[session_id]);
    }

    // STARTED goes out before any output delta can (the watcher does not exist
    // yet), so a client never sees output for a task it was not told about.
    if (handles.emit_task_event) {
        BackgroundTaskEvent event;
        event.task_id = task->id;
        event.tool_use_id = handles.tool_use_id;
        event.command = command;
        event.pid = proc.pid;
        event.state = BackgroundTaskEvent::State::kStarted;
        handles.emit_task_event(event);
    }

    // Detached, never joined: the task may outlive the turn, the worker, and
    // the session. Everything the thread touches is owned -- the shared
    // TaskState (passed by COPY, since the id is still read below), the
    // handles (channels-backed), copies of settings and timeout -- so no
    // teardown path can strand it on freed memory. The registry itself is a
    // process-lifetime static inside this .so.
    const StartedTask started{task->id, proc.pid};
    // Captured before the move: if thread construction throws, the moved-from
    // handles are the only things that could report the failure, and they are
    // gone by then.
    common::Logger* logger =
        handles.logger ? handles.logger : &common::DefaultLogger();
    auto emit_task_event = handles.emit_task_event;
    const std::string tool_use_id = handles.tool_use_id;
    // std::thread's constructor allocates and can throw (resource exhaustion)
    // -- and by this point the child is already forked, setsid'd, announced as
    // STARTED, and registered in the sweep set. Contained here rather than in
    // the watcher's guard, because the watcher thread is precisely what failed
    // to come into existence: kill the group, reap, forget the pgid, and mark
    // the task terminal so CountRunning never wedges the session at its task
    // limit on a task that will never report.
    try {
        std::thread(WatchTask, task, proc, settings, timeout_ms, std::move(handles)).detach();
    } catch (...) {
        try {
            bool reaped = false;
            ReapAfterKill(proc.pid, reaped);  // SIGKILLs the group, reaps bounded
        } catch (...) {
        }
        try {
            ForgetPgid(proc.pid);
        } catch (...) {
        }
        task->state.store(static_cast<int>(BackgroundTaskEvent::State::kFailed),
                          std::memory_order_release);
        if (emit_task_event) {
            try {
                BackgroundTaskEvent event;
                event.task_id = task->id;
                event.tool_use_id = tool_use_id;
                event.command = task->command;
                event.pid = task->pid;
                event.state = BackgroundTaskEvent::State::kFailed;
                emit_task_event(event);
            } catch (...) {
            }
        }
        logger->error("bash[{}] background {}: watcher thread could not start; task killed",
                      task->session_id, task->id);
        *error = "failed to start background task (could not create watcher thread)";
        return std::nullopt;
    }

    return started;
}

std::vector<BackgroundTaskRegistry::TaskInfo> BackgroundTaskRegistry::List(
    const std::string& session_id) {
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
        info.command = task->command;
        info.pid = task->pid;
        info.state = static_cast<BackgroundTaskEvent::State>(
            task->state.load(std::memory_order_acquire));
        info.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - task->started_at)
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

std::size_t BackgroundTaskRegistry::CountRunning(const std::string& session_id) {
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = tasks_.find(session_id); it != tasks_.end()) {
            for (const auto& task : it->second) {
                const auto state = static_cast<BackgroundTaskEvent::State>(
                    task->state.load(std::memory_order_acquire));
                if (StateIsRunning(state)) ++count;
            }
        }
    }
    return count;
}

void BackgroundTaskRegistry::EraseSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.erase(session_id);
}

}  // namespace minimal::tools_bash
