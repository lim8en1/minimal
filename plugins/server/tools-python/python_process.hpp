#pragma once

// Process machinery for the python interpreter and the short-lived helper
// commands (venv creation, package installation). The dangerous parts -- fork
// + execve (never posix_spawn: setrlimit must run in the child before exec
// and is not async-signal-safe), setsid so pgid == pid and one killpg reaches
// the whole tree, and the process-global pgid set with the atexit SIGKILL
// sweep as the last-resort backstop for server exit -- live in the SHARED
// sidecar core (minimal/sidecar/process.hpp), where tools-bash and every
// future process-spawning plugin use the same one copy. What remains here is
// python's own contract: the interpreter launch shape (protocol stdin/stdout
// isolated from the real stdout/stderr via a diagnostics pipe) and the
// captured one-shot helper run, both expressed over the generic pipe-plan
// launcher and the generic drain-and-reap runner.

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

#include <unistd.h>

#include "minimal/common/logger.hpp"
#include "minimal/sidecar/process.hpp"

namespace minimal::tools_python {

// RAII fd owned by whichever runner drains the launched process: the sidecar
// core's one implementation.
using ScopedFd = minimal::sidecar::ScopedFd;

// A long-lived interpreter process. stdin/out are the PROTOCOL channels;
// diag is what the child's real stdout+stderr (fd 1/2) point at -- executed
// code that writes to the real fds, and Python-level pre-loop failures, land
// there instead of corrupting the protocol stream. stream_fd is the live
// stdout stream (unframed write-through from the running request's capture);
// user_in_fd is the parent's write end of the user stdin the driver's
// sys.stdin reads. The caller owns all five ends through ScopedFd.
struct LaunchedInterpreter {
    bool ok = false;
    std::string spawn_error;
    pid_t pid = -1;
    int in_fd = -1;      // parent writes request lines here
    int out_fd = -1;     // parent reads response lines here
    int diag_fd = -1;    // parent drains diagnostics here (capped, on death)
    int stream_fd = -1;  // parent drains live stdout deltas here
    int user_in_fd = -1; // parent writes python_input lines here
    // Non-empty when Launch() had to clamp an rlimit backstop to the host's
    // hard cap (see ClampRlimit); log it -- the child runs degraded, not dead.
    std::string warnings;
    // The interpreter runs under a MemoryMax scope (sidecar/memory_scope.hpp):
    // a SIGKILL death is then likely the ceiling, and says so.
    bool memory_scoped = false;
};

// fork + execve of `argv` with `cwd` as the working directory, over the
// sidecar core's pipe-plan launcher. The child setsid()s (pgid == pid:
// killpg reaches the whole tree), applies the RLIMIT_FSIZE backstop and --
// when cpu_seconds > 0 -- an RLIMIT_CPU backstop, and runs with `env` (a
// full environment map, not a delta). fd 1/2 go to the diagnostics pipe; fd
// 0 is the protocol stdin; the protocol stdout is dup2'd to kProtoStdoutFd
// and the live stdout stream to kStreamFd, both with FD_CLOEXEC cleared (the
// driver writes the protocol on kProtoStdoutFd and streams on kStreamFd, so
// executed code writing to "stdout" cannot corrupt the JSON lines); the
// user stdin pipe is dup2'd to kUserStdinFd with FD_CLOEXEC cleared (the
// driver's sys.stdin reads it).
// The first fds after stdio, in order.
constexpr int kProtoStdoutFd = STDERR_FILENO + 1;
constexpr int kStreamFd = kProtoStdoutFd + 1;
constexpr int kUserStdinFd = kStreamFd + 1;
LaunchedInterpreter LaunchInterpreter(const std::vector<std::string>& argv,
                                      const std::filesystem::path& cwd,
                                      const std::map<std::string, std::string>& env,
                                      std::int64_t cpu_seconds, std::int64_t file_size_bytes,
                                      int memory_max_percent, common::Logger& logger);

// One short-lived helper command (venv creation, package install): runs argv
// to completion with merged output captured (head+tail capped), the whole
// group killed at the deadline, and a bounded reap. exit_status is -1 when the
// command was killed or could not be reaped.
struct CapturedRun {
    bool ok = false;      // process was spawned and reaped
    std::string error;    // spawn failure reason when !ok
    int exit_status = -1;
    std::string output;   // merged stdout+stderr, head+tail capped
};
CapturedRun RunCaptured(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::map<std::string, std::string>& env,
                        std::int64_t timeout_seconds, std::size_t max_output_bytes);

// Group kill / pgid tracking / PATH resolution: the sidecar core's one
// implementation (and one sweep, shared with every other spawning plugin).
inline void KillProcessGroup(pid_t pid) { return minimal::sidecar::KillProcessGroup(pid); }
inline bool ReapAfterKill(pid_t pid) { return minimal::sidecar::ReapAfterKill(pid); }
inline void TrackPgid(pid_t pid) { return minimal::sidecar::TrackPgid(pid); }
inline void ForgetPgid(pid_t pid) { return minimal::sidecar::ForgetPgid(pid); }
inline std::string ResolveOnPath(const std::string& name) {
    return minimal::sidecar::ResolveOnPath(name);
}

}  // namespace minimal::tools_python
