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
#include "minimal/common/logger.hpp"
#include "minimal/common/units.hpp"

#include "bash_settings.hpp"
#include "bash_session_state.hpp"
#include "policy.hpp"

namespace minimal::tools_bash {

// The shared capped-output buffer and fd guard: one implementation, in the
// sidecar core.
using CappedBuffer = minimal::sidecar::CappedBuffer;
using ScopedFd = minimal::sidecar::ScopedFd;

// kStateFd, kMaxStatePayloadBytes and kIdleAfterReap: tools-bash/policy.hpp.
// On a state-payload overflow the payload is discarded WHOLESALE, preserving
// the all-or-nothing rule: adopting a truncated prefix would silently apply
// partial environment state.

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

// Joins captured output with a status footer and, when non-empty, a
// detached-grandchild teaching note -- shared by the sync (bash_tool.cpp) and
// background (background_registry.cpp) result paths so the composition rule
// (footer/note ordering, when to skip the separator) only exists once.
std::string ComposeResult(const std::string& output, const std::string& footer,
                           const std::string& grandchild_note);

// Payload: <absolute cwd> NUL (KEY=VALUE NUL)*
//
// Validated rather than trusted. A command may write to fd 9 itself -- by
// accident or otherwise -- and session state must not be corruptible that way.
// Anything unexpected leaves the previous state untouched, which is also what a
// command that exits before the trailer runs produces (an empty payload).
ShellState ParseStatePayload(const std::string& payload);

// The launch shape shared by BOTH the synchronous path (this file's
// LaunchProcess) and the background task runner (background_registry.cpp's
// BackgroundTaskRegistry::Start): merged stdout+stderr on parent_fds[0], the
// fd-9 state channel on parent_fds[1] -- a passthrough fd, so its CLOEXEC
// must be cleared explicitly or exec closes it. `argv` differs between the
// two callers only in the wrapped command string; everything else (cwd, env,
// pipes, rlimit backstops) is identical, so this is the ONE place that shape
// is expressed -- any future rlimit or state-fd change is made here, not
// twice.
//
// With `settings.memory_max_percent` set and a usable systemd user manager
// (memory_scope.hpp), argv runs the shell inside a transient MemoryMax scope;
// `logger` reports the one-time probe.
minimal::sidecar::LaunchSpec BuildLaunchSpec(const std::string& wrapped, const SessionShellState& state,
                                             const BashSettings& settings, std::int64_t timeout_ms,
                                             common::Logger& logger);

// fork + execve of the configured shell with `wrapped`, over the sidecar
// core's pipe-plan launcher. The child setsid()s, so pgid == pid and
// KillProcessGroup reaches the whole tree. stdin is /dev/null. Returns the
// sidecar core's own result type directly (not a bash-specific wrapper) --
// the caller (bash_tool.cpp's RunProcess) hands it straight to DrainAndReap,
// which also takes a `Launched`; the previous shape (a LaunchedProcess with
// raw `int out_fd`/`state_fd`) forced every caller to `.release()` the real
// ScopedFds and re-wrap fresh ones just to call DrainAndReap, a window where
// a throw between the two would leak both fds and the child.
minimal::sidecar::Launched LaunchProcess(const std::string& wrapped, const SessionShellState& state,
                                         const BashSettings& settings, std::int64_t timeout_ms,
                                         common::Logger& logger);

// "[terminated by signal N]", plus a hint when the command ran under a memory
// ceiling and died of SIGKILL -- what the kernel's OOM kill inside the scope
// looks like (a deadline or cancellation is reported before this is reached).
// Shared by the sync and background result paths.
std::string SignalFooter(int sig, const BashSettings& settings, common::Logger& logger);

// Group kill / pgid tracking: the sidecar core's one implementation (and one
// sweep, shared with every other spawning plugin).
inline void KillProcessGroup(pid_t pid) { return minimal::sidecar::KillProcessGroup(pid); }
inline void TrackPgid(pid_t pid) { return minimal::sidecar::TrackPgid(pid); }
inline void ForgetPgid(pid_t pid) { return minimal::sidecar::ForgetPgid(pid); }

}  // namespace minimal::tools_bash
