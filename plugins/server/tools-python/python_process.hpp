#pragma once

// Process machinery for the python interpreter and the short-lived helper
// commands (venv creation, package installation). One .so, one implementation
// of the dangerous parts -- fork + execve (never posix_spawn: setrlimit must
// run in the child before exec and is not async-signal-safe), setsid so
// pgid == pid and one killpg reaches the whole tree, and the process-global
// pgid set with an atexit SIGKILL sweep as the last-resort backstop for server
// exit (detached threads die with the process; their setsid'd children would
// otherwise be orphaned). Modeled on tools-bash's bash_process.cpp, which
// serves a different contract (a /bin/sh wrapper with a state trailer); this
// plugin execs argv directly and never needs shell semantics.

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace minimal::tools_python {

// RAII fd owned by whichever runner drains the launched process.
class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd();
    ScopedFd(ScopedFd&& other) noexcept;
    ScopedFd& operator=(ScopedFd&& other) noexcept;
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(ScopedFd const&) = delete;

    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset();

private:
    int fd_ = -1;
};

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

// fork + execve of `argv` with `cwd` as the working directory. The child
// setsid()s (pgid == pid: killpg reaches the whole tree), applies the
// RLIMIT_FSIZE backstop and -- when cpu_seconds > 0 -- an RLIMIT_CPU backstop,
// and runs with `env` (a full environment map, not a delta). fd 1/2 go to the
// diagnostics pipe; fd 0 is the protocol stdin; the protocol stdout is dup2'd
// to kProtoStdoutFd with FD_CLOEXEC cleared (the driver reads/writes the
// protocol on fds 0 and kProtoStdoutFd, so executed code writing to "stdout"
// cannot corrupt the JSON lines).
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

// SIGKILL the process group, plus the pid itself to cover the fork-to-setsid
// window. Does NOT reap; the caller waitpids.
void KillProcessGroup(pid_t pid);

// Resolves a program name against the PROCESS's PATH (absolute paths and
// anything containing '/' pass through). execve does no PATH resolution, so
// every launch helper must call this on argv[0]. Empty on failure.
std::string ResolveOnPath(const std::string& name);

// The process-global pgid set + atexit sweep (see file comment). Tracked at
// launch, forgotten on reap, so the sweep holds only LIVE groups.
void TrackPgid(pid_t pid);
void ForgetPgid(pid_t pid);

// Blocking reap bounded by the SIGKILL already pending on the target.
void ReapAfterKill(pid_t pid);

}  // namespace minimal::tools_python
