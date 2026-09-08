#pragma once

// Process machinery shared by the synchronous bash tool and the background
// task runner. The dangerous parts -- fork + execve (never posix_spawn:
// setrlimit is not async-signal-safe, so the rlimits must be set in the child
// before exec), setsid for whole-group killpg, and the process-global pgid
// set with its atexit sweep -- live in the SHARED sidecar core
// (minimal/sidecar/process.hpp), where tools-python and every future
// process-spawning plugin use the same one copy. What remains here is bash's
// own contract: the /bin/sh wrapper that reports final cwd/env on fd 9, and
// the launch shape (merged stdout+stderr, plus the state channel) expressed
// over the generic pipe-plan launcher. See ARCHITECTURE.md "Core tools" for
// the security rationale (no allowlist; the permission gate is the boundary).

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>

#include <sys/types.h>

#include "minimal/sidecar/capped_buffer.hpp"
#include "minimal/sidecar/process.hpp"

#include "bash_settings.hpp"
#include "bash_session_state.hpp"

namespace minimal::tools_bash {

// The shared capped-output buffer and fd guard: one implementation, in the
// sidecar core.
using CappedBuffer = minimal::sidecar::CappedBuffer;
using ScopedFd = minimal::sidecar::ScopedFd;

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

// The parent-side handles of a launched shell. The fds are owned by the
// caller (ScopedFd); both read ends are O_CLOEXEC and belong to THIS
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

// fork + execve of the configured shell with `wrapped`, over the sidecar
// core's pipe-plan launcher. The child setsid()s, so pgid == pid and
// KillProcessGroup reaches the whole tree. stdin is /dev/null.
LaunchedProcess LaunchProcess(const std::string& wrapped, const SessionShellState& state,
                              const BashSettings& settings, std::int64_t timeout_ms);

// Group kill / pgid tracking: the sidecar core's one implementation (and one
// sweep, shared with every other spawning plugin).
inline void KillProcessGroup(pid_t pid) { return minimal::sidecar::KillProcessGroup(pid); }
inline void TrackPgid(pid_t pid) { return minimal::sidecar::TrackPgid(pid); }
inline void ForgetPgid(pid_t pid) { return minimal::sidecar::ForgetPgid(pid); }

}  // namespace minimal::tools_bash
