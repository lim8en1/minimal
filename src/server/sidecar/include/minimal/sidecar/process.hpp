#pragma once

// Sidecar process machinery: the one implementation of the dangerous parts
// (fork + execve, never posix_spawn -- setrlimit is not async-signal-safe, so
// the rlimits must run in the child before exec), shared by every plugin that
// spawns processes. Before this module, tools-bash and tools-python each
// carried a near-identical copy (python's was literally "modeled on" bash's);
// both now delegate here. Living in the SHARED server core matters beyond
// deduplication: the process-global pgid set and its atexit sweep are globals,
// and one copy means ONE sweep that knows about every child in the process,
// not two independent sets that each kill only half the orphans.
//
// The launcher is shaped around pipes, because that is all a child process
// ever negotiates: the caller describes which pipe ends land on which child
// fds, and the parent keeps the opposite ends. Every caller's shape -- bash's
// merged stdout+stderr plus an fd-9 state channel, the python interpreter's
// protocol stdin/stdout plus a diagnostics pipe, a one-shot captured run --
// is a PipePlan list over the same fork/exec/rlimit skeleton.
//
// See ARCHITECTURE.md "Core tools" for the security rationale (no allowlist;
// the permission gate is the boundary).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>

namespace minimal::sidecar {

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

// One pipe between parent and child. The child receives `child_end`, dup2'd
// onto every fd in `dups` (a merged stdout+stderr is two dups of one write
// end). `clear_cloexec` applies to the child's copy AFTER the dup2: dup2 is a
// no-op when source and target are the same fd, so a passthrough fd > 2 (the
// bash state channel on fd 9, the python protocol stdout on fd 3) needs the
// explicit clear or exec closes it. The parent automatically keeps the
// opposite end.
struct PipePlan {
    enum class ChildEnd { kRead, kWrite };
    struct Dup {
        int child_fd = -1;
        bool clear_cloexec = false;
    };
    ChildEnd child_end = ChildEnd::kWrite;
    std::vector<Dup> dups;
};

struct LaunchSpec {
    // argv[0] is resolved against the PROCESS's PATH when it contains no '/'
    // (execve does no PATH resolution); resolution failure is a spawn error,
    // reported in the parent, before any fork.
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    // The child's FULL environment (not a delta): envp is built solely from
    // this map, sorted for deterministic order.
    std::map<std::string, std::string> env;
    // Parent ends come back in this order: parent_fds[i] is the end of
    // pipes[i] the child did NOT receive.
    std::vector<PipePlan> pipes;
    // When no Dup targets fd 0, the child's stdin is /dev/null -- without
    // this, a child inherits the server's stdin and any command that reads
    // fd 0 blocks until the deadline.
    std::int64_t cpu_seconds = 0;  // 0 = unlimited; clamped to the host's hard cap by Launch()
    std::int64_t file_size_bytes = 1LL << 30;  // RLIMIT_FSIZE backstop; clamped likewise
};

// The parent-side handles of a launched process. All fds are O_CLOEXEC and
// belong to THIS process only. The child setsid()s, so pgid == pid and
// KillProcessGroup reaches the whole tree; the pid is tracked in the
// process-global pgid set (atexit sweep) from launch until ForgetPgid.
struct Launched {
    bool ok = false;
    std::string spawn_error;
    pid_t pid = -1;
    std::vector<ScopedFd> parent_fds;
    // Non-empty when a backstop was clamped to the host's hard cap (see
    // ClampRlimit): one line per clamp, naming the limit, what was requested,
    // and what the child will actually run under. Callers log it; the spawn
    // still succeeds -- a degraded backstop beats a dead child.
    std::string warnings;
};

Launched Launch(const LaunchSpec& spec);

// The largest value setrlimit(resource) can actually set in a child of THIS
// process: `requested` clamped to `hard_cap` (an RLIM_INFINITY hard cap never
// clamps). The launcher calls this with the limit's inherited rlim_max so a
// request above it degrades to the cap instead of dying at the child's
// setrlimit check (EPERM: a hard limit cannot be raised). Appends a
// human-readable line to `*warnings` when it clamps; `warnings` may be null.
rlim_t ClampRlimit(rlim_t requested, rlim_t hard_cap, std::string* warnings);

// SIGKILL the process group, plus the pid itself to cover the fork-to-setsid
// window (between them the child's pgid is still ours and killpg finds no
// such group). Does NOT reap; the caller waitpids.
void KillProcessGroup(pid_t pid);

// SIGKILL the process group, then bounded-reap: WNOHANG polling for up to
// 5 s (a D-state child never becomes reapable, and a blocking waitpid would
// park a session worker forever). Returns false when the child was not
// reaped within the budget -- the caller decides whether that is worth
// logging; the child is a zombie at worst, never a hang. ECHILD counts as
// reaped: the child is gone (reaped elsewhere), no residue, not a failure.
bool ReapAfterKill(pid_t pid);

// The process-global pgid set + atexit sweep. THE SWEEP IS A BACKSTOP FOR
// PROCESS EXIT, not a teardown mechanism: only process exit orphans setsid'd
// children (detached watchers die with the process; their children would be
// reparented and run to completion). Pids are tracked at launch and forgotten
// on reap, so the set holds only LIVE process groups -- no pid-reuse exposure
// at sweep time.
void TrackPgid(pid_t pid);
void ForgetPgid(pid_t pid);

// Resolves a program name against the PROCESS's PATH (absolute paths and
// anything containing '/' pass through). Empty on failure.
std::string ResolveOnPath(const std::string& name);

}  // namespace minimal::sidecar
