#pragma once

// Process machinery shared by the synchronous bash tool and the background
// task runner. One .so, one implementation of the dangerous parts: fork +
// execve (never posix_spawn -- setrlimit is not async-signal-safe, so the
// rlimits must be set in the child before exec), setsid for whole-group
// killpg, the /bin/sh wrapper that reports final cwd/env on fd 9, and the
// head+tail capped output buffer. See ARCHITECTURE.md "Core tools" for the
// security rationale (no allowlist; the permission gate is the boundary).

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

#include <sys/types.h>

#include "bash_settings.hpp"
#include "bash_session_state.hpp"

namespace minimal::tools_bash {

// --- Capped output buffer --------------------------------------------------------
//
// Head (1/4 of the cap) + tail (3/4): in a long build log both the first error
// and the final status matter. This bounds only what is PERSISTED into the
// ToolResultBlock; live streaming through emit_output is uncapped because that
// channel is lossy by design -- the session's outbound queue is the backpressure
// boundary, and the authoritative record is the capped result block here.
class CappedBuffer {
public:
    explicit CappedBuffer(std::int64_t cap)
        : head_cap_(static_cast<std::size_t>(cap / 4)),
          tail_cap_(static_cast<std::size_t>(cap - cap / 4)) {}

    void Append(std::string_view chunk);
    std::string Render();

private:
    void Compact();

    std::size_t head_cap_, tail_cap_;
    std::string head_, tail_;
    std::size_t dropped_ = 0;
};

// The fd the wrapper writes session state to. Any fd can collide with something
// the command does itself, so correctness rests on validating the payload rather
// than on picking a lucky number -- see ParseStatePayload.
constexpr int kStateFd = 9;

// The state payload is `pwd -P` plus `env -0` -- kilobytes at most. Bounded so a
// command pumping fd 9 (`yes >&9`) cannot grow one string without limit for the
// whole timeout; on overflow the payload is discarded WHOLESALE, preserving the
// all-or-nothing rule: adopting a truncated prefix would silently apply partial
// environment state.
constexpr std::size_t kMaxStatePayloadBytes = 1 << 20;  // 1 MiB

struct ShellState {
    std::filesystem::path cwd;
    std::map<std::string, std::string> env;
    bool ok = false;
};

// `cd` and `export` inside `sh -c` die with the subshell, so the shell reports
// its final cwd and environment on a side channel and the caller adopts them.
// This is what lets the process stay per-call (no long-lived interpreter to
// corrupt, hang, or leak) while `cd`/`export` still chain across calls.
std::string WrapCommand(const std::string& command);

// Payload: <absolute cwd> NUL (KEY=VALUE NUL)*
//
// Validated rather than trusted. A command may write to fd 9 itself -- by
// accident or otherwise -- and session state must not be corruptible that way.
// Anything unexpected leaves the previous state untouched, which is also what a
// command that exits before the trailer runs produces (an empty payload).
ShellState ParseStatePayload(const std::string& payload);

// The parent-side handles of a launched process group. The fds are owned by the
// caller (ScopedFd below); both read ends are O_CLOEXEC and belong to THIS
// process only. `state_fd` carries the wrapper's cwd/env trailer -- a caller
// that does not want state (the background runner) still DRAINS it, because a
// child blocked writing a full pipe never reaches its own exit.
struct LaunchedProcess {
    bool ok = false;
    std::string spawn_error;
    pid_t pid = -1;
    int out_fd = -1;    // stdout+stderr, merged, in order
    int state_fd = -1;  // the fd-9 state channel
};

// fork + execve of the configured shell with `wrapped`. The child setsid()s, so
// pgid == pid and KillProcessGroup reaches the whole tree. stdin is /dev/null.
// Tracks the pid in the process-global pgid set (see below).
LaunchedProcess LaunchProcess(const std::string& wrapped, const SessionShellState& state,
                              const BashSettings& settings, std::int64_t timeout_ms);

// SIGKILL the process group, plus the pid itself to cover the fork-to-setsid
// window (between them the child's pgid is still ours and killpg finds no such
// group). Does NOT reap; the caller waitpids.
void KillProcessGroup(pid_t pid);

// --- The process-global pgid set and the atexit sweep -------------------------------
//
// THE SWEEP IS A BACKSTOP FOR PROCESS EXIT, not a teardown mechanism: a
// session's terminate path works through the background watcher's
// session_lifetime polls, which is prompt and ordered. Only process exit
// orphans setsid'd children -- detached watchers die with the process, and
// their children would be reparented and run to completion -- so an atexit
// handler (registered on first LaunchProcess) sweeps synchronously, when there
// is nothing left to race.
//
// Pids are tracked at launch and forgotten on reap, so the set holds only LIVE
// process groups -- no pid-reuse exposure at sweep time. Foreground tasks are
// tracked too: they die with their tool call, but only after the pid is known.

void TrackPgid(pid_t pid);
void ForgetPgid(pid_t pid);

// RAII fd owned by whichever runner drains the launched process.
class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd();
    ScopedFd(ScopedFd&& other) noexcept;
    ScopedFd& operator=(ScopedFd&& other) noexcept;
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    // Yields ownership: returns the fd and stops tracking it.
    int release() { return std::exchange(fd_, -1); }
    void reset();

private:
    int fd_ = -1;
};

}  // namespace minimal::tools_bash
