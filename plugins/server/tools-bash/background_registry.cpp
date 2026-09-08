#include "background_registry.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <utility>

#include "minimal/sidecar/drain.hpp"

namespace minimal::tools_bash {
namespace {

using minimal::sidecar::PipeSink;
using minimal::sidecar::TaskRegistry;
using tools::BackgroundTaskEvent;

// Kills the group, then reaps with a BOUND. "Bounded by SIGKILL" does not
// hold for a process stuck in uninterruptible disk sleep (D state): SIGKILL
// stays pending indefinitely and a blocking call would park the caller
// forever. Poll WNOHANG with a generous ceiling instead; past it, `reaped`
// stays false and the caller reports the status as undetermined rather than
// hanging. Used by the containment paths (an abnormal watcher exit, a
// watcher thread that never came into existence), where there is no drain
// loop to bound the wait anymore.
constexpr int kPostKillReapMs = 5000;
void BoundedReapAfterKill(pid_t pid, bool& reaped) {
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
        poll(nullptr, 0, 25);
    }
}

// The watcher body. Runs detached under the shared registry's containment
// wrapper; owns its state through the registry's shared TaskState and owned
// copies of the launch handles, so a session dying underneath it is a handled
// condition, never a dangle.
//
// Lifecycle: drain stdout+stderr (capped; streamed live) and DISCARD the state
// channel (a background task's final cwd/env is stale by the time it reports;
// adopting it would stomp what foreground calls set up in between -- but the
// channel must still be drained, or a child blocked writing a full pipe never
// reaches its own exit), reap, classify, emit the lifecycle event, inject the
// result -- the last three through TaskRegistry::Finish, so the ordering is
// the shared one.
void WatchBackgroundTask(TaskRegistry::TaskState& task,
                         minimal::sidecar::Launched launched, const BashSettings& settings,
                         std::int64_t timeout_ms, const TaskRegistry::WatchHandles& handles) {
    common::Logger& logger = handles.logger ? *handles.logger : common::DefaultLogger();

    std::vector<PipeSink> sinks(2);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = settings.max_output_bytes;
    // Live channel to the operator, keyed to the original tool_use_id; lossy
    // by design, the capped record lives in the sink.
    sinks[0].emit = handles.emit_output;
    sinks[1].mode = PipeSink::Mode::kDiscard;

    minimal::sidecar::DrainOptions options;
    options.timeout_ms = timeout_ms;

    // session_lifetime is the ONLY stop source (no turn CancelToken is held).
    const auto outcome = minimal::sidecar::DrainAndReap(
        std::move(launched), std::move(sinks), options,
        [&] { return TaskRegistry::SessionGone(handles); });

    // Classification. Session death wins over the deadline (the drain loop
    // checks it first too): "the session ended" is the more useful report for
    // the one case where nobody is listening to the other.
    auto exit = BackgroundTaskEvent::State::kStarted;  // kStarted == still running
    if (outcome.cancelled) {
        exit = BackgroundTaskEvent::State::kKilled;
    } else if (outcome.timed_out) {
        exit = BackgroundTaskEvent::State::kTimedOut;
    } else if (outcome.io_error) {
        exit = BackgroundTaskEvent::State::kFailed;
    } else if (outcome.reap_error) {
        // ECHILD: the pid was reaped out from under us; the status is
        // meaningless and must not read as a clean exit.
        exit = BackgroundTaskEvent::State::kFailed;
    } else if (WIFSIGNALED(outcome.wait_status)) {
        exit = BackgroundTaskEvent::State::kFailed;
    } else {
        const int code =
            WIFEXITED(outcome.wait_status) ? WEXITSTATUS(outcome.wait_status) : -1;
        exit = code == 0 ? BackgroundTaskEvent::State::kCompleted
                         : BackgroundTaskEvent::State::kFailed;
    }

    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - task.started_at)
            .count();
    switch (exit) {
        case BackgroundTaskEvent::State::kCompleted:
            logger.info("bash[{}] background {} ok: '{}' ({} ms)", task.session_id, task.id,
                        task.label, duration_ms);
            break;
        case BackgroundTaskEvent::State::kKilled:
            logger.warn("bash[{}] background {}: session ended; '{}' killed ({} ms)",
                        task.session_id, task.id, task.label, duration_ms);
            break;
        default:
            logger.warn("bash[{}] background {} ended (state {}): '{}' ({} ms)",
                        task.session_id, task.id, static_cast<int>(exit), task.label,
                        duration_ms);
            break;
    }

    const std::string output = outcome.sinks[0].text;
    const auto with_output = [&](const std::string& footer) {
        return output.empty() ? footer : output + "\n" + footer;
    };
    std::string text;
    bool is_error = false;
    switch (exit) {
        case BackgroundTaskEvent::State::kCompleted:
            text = with_output("[background task " + task.id + " completed]");
            break;
        case BackgroundTaskEvent::State::kFailed: {
            is_error = true;
            if (outcome.reap_error) {
                text = "[could not determine exit status (process already reaped)]";
            } else if (WIFSIGNALED(outcome.wait_status)) {
                text = with_output("[terminated by signal " +
                                   std::to_string(WTERMSIG(outcome.wait_status)) + "]");
            } else if (outcome.io_error) {
                text = "[background task " + task.id + " failed reading output]";
            } else {
                const int code =
                    WIFEXITED(outcome.wait_status) ? WEXITSTATUS(outcome.wait_status) : -1;
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

    TaskRegistry::Finish(task, handles, exit, std::move(text), is_error);
}

}  // namespace

std::optional<BackgroundTaskRegistry::StartedTask> BackgroundTaskRegistry::Start(
    const std::string& session_id, const std::string& command, const SessionShellState& state,
    const BashSettings& settings, std::int64_t timeout_ms, WatchHandles handles,
    std::string* error) {
    minimal::sidecar::LaunchSpec spec;
    // Same launch shape as the synchronous path (bash_process.cpp): merged
    // stdout+stderr, plus the fd-9 state channel -- drained and discarded
    // here, since a background task's final cwd/env is stale by the time it
    // reports. The state fd is a passthrough fd, so its CLOEXEC must be
    // cleared explicitly or exec closes it.
    spec.argv = {settings.shell, "-c", WrapCommand(command)};
    spec.cwd = state.cwd;
    spec.env = state.env;
    spec.pipes = {
        minimal::sidecar::PipePlan{
            minimal::sidecar::PipePlan::ChildEnd::kWrite,
            {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
        minimal::sidecar::PipePlan{
            minimal::sidecar::PipePlan::ChildEnd::kWrite,
            {{kStateFd, true}}},
    };
    spec.cpu_seconds = timeout_ms / 1000 + 5;
    spec.file_size_bytes = settings.max_file_size_bytes;

    auto launched = minimal::sidecar::Launch(spec);
    if (!launched.ok) {
        *error = "failed to start shell: " + launched.spawn_error;
        return std::nullopt;
    }
    const int pid = launched.pid;

    TaskRegistry::StartArgs args;
    args.session_id = session_id;
    args.label = command;
    args.pid = pid;
    args.id_prefix = "bg";
    args.max_running = static_cast<std::size_t>(settings.max_background_tasks);
    // The watcher's process handles are non-copyable (ScopedFd members), and
    // the registry's Body is a std::function (copyable callable). Wrap them:
    // the shared_ptr IS the watcher's ownership of the process handles, the
    // same lifetime story the registry uses for its TaskState.
    auto handles_box =
        std::make_shared<minimal::sidecar::Launched>(std::move(launched));
    args.body = [handles_box, settings, timeout_ms](TaskRegistry::TaskState& task,
                                                    const TaskRegistry::WatchHandles& watch_handles) {
        WatchBackgroundTask(task, std::move(*handles_box), settings, timeout_ms,
                            watch_handles);
    };
    // Containment for an abnormal watcher exit: the drain-and-reap normally
    // leaves nothing live (it reaps and forgets the pgid), but a throw out of
    // the body skips all of that -- a live process group plus a stale pgid
    // registration, which pid reuse turns into a wrong-target kill at process
    // exit. Best-effort kill + bounded reap; idempotent.
    args.cleanup = [pid] {
        try {
            bool reaped = false;
            BoundedReapAfterKill(pid, reaped);
        } catch (...) {
        }
        try {
            ForgetPgid(pid);
        } catch (...) {
        }
    };
    args.handles = std::move(handles);

    auto id = registry_.Start(std::move(args), error);
    if (!id) return std::nullopt;
    return StartedTask{*id, pid};
}

std::vector<BackgroundTaskRegistry::TaskInfo> BackgroundTaskRegistry::List(
    const std::string& session_id) {
    std::vector<TaskInfo> out;
    for (const auto& info : registry_.List(session_id)) {
        out.push_back(TaskInfo{info.id, info.label, info.pid, info.state, info.elapsed_ms});
    }
    return out;
}

std::size_t BackgroundTaskRegistry::CountRunning(const std::string& session_id) {
    return registry_.CountRunning(session_id);
}

void BackgroundTaskRegistry::EraseSession(const std::string& session_id) {
    registry_.EraseSession(session_id);
}

}  // namespace minimal::tools_bash
