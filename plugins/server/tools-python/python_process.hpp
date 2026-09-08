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

#include "minimal/sidecar/process.hpp"

namespace minimal::tools_python {

// RAII fd owned by whichever runner drains the launched process: the sidecar
// core's one implementation.
using ScopedFd = minimal::sidecar::ScopedFd;

// A long-lived interpreter process. stdin/out are the PROTOCOL channels;
// diag is what the child's real stdout+stderr (fd 1/2) point at -- executed
// code that writes to the real fds, and Python-level pre-loop failures, land
// there instead of corrupting the protocol stream. The caller owns all three
// read/write ends through ScopedFd.
struct LaunchedInterpreter {
    bool ok = false;
    std::string spawn_error;
    pid_t pid = -1;
    int in_fd = -1;    // parent writes request lines here
    int out_fd = -1;   // parent reads response lines here
    int diag_fd = -1;  // parent drains diagnostics here (capped, on death)
};

// fork + execve of `argv` with `cwd` as the working directory, over the
// sidecar core's pipe-plan launcher. The child setsid()s (pgid == pid:
// killpg reaches the whole tree), applies the RLIMIT_FSIZE backstop and --
// when cpu_seconds > 0 -- an RLIMIT_CPU backstop, and runs with `env` (a
// full environment map, not a delta). fd 1/2 go to the diagnostics pipe; fd
// 0 is the protocol stdin; the protocol stdout is dup2'd to kProtoStdoutFd
// with FD_CLOEXEC cleared (the driver reads/writes the protocol on fds 0 and
// kProtoStdoutFd, so executed code writing to "stdout" cannot corrupt the
// JSON lines).
constexpr int kProtoStdoutFd = 3;
LaunchedInterpreter LaunchInterpreter(const std::vector<std::string>& argv,
                                      const std::filesystem::path& cwd,
                                      const std::map<std::string, std::string>& env,
                                      std::int64_t cpu_seconds, std::int64_t file_size_bytes);

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
inline void ReapAfterKill(pid_t pid) { return minimal::sidecar::ReapAfterKill(pid); }
inline void TrackPgid(pid_t pid) { return minimal::sidecar::TrackPgid(pid); }
inline void ForgetPgid(pid_t pid) { return minimal::sidecar::ForgetPgid(pid); }
inline std::string ResolveOnPath(const std::string& name) {
    return minimal::sidecar::ResolveOnPath(name);
}

}  // namespace minimal::tools_python
