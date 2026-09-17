#include "minimal/sidecar/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

namespace minimal::sidecar {

namespace {
// Post-kill reap ceiling: how long a bounded waitpid polls before giving up.
constexpr int kPostKillReapMs = 5000;
}  // namespace

// --- ScopedFd ------------------------------------------------------------------------

ScopedFd::~ScopedFd() { reset(); }

ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept {
    reset();
    fd_ = std::exchange(other.fd_, -1);
    return *this;
}

void ScopedFd::reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

// --- The pgid set and the atexit sweep -------------------------------------------------

namespace {

std::mutex g_pgids_mu;
std::set<pid_t>& Pgids() {
    // Leaked deliberately: SweepProcessGroups is registered with atexit at
    // first TrackPgid (during main), so it runs BEFORE the function-local
    // static's destructor is reached -- destruction order is reverse
    // registration. A translation-unit static (or function-local static) set
    // would already be destroyed when the sweep runs: UB -- an exit hang, a
    // crash, or killpg/kill firing at recycled pids. A set that is never
    // freed keeps the sweep sound to the last instruction of the process.
    static std::set<pid_t>* const pgids = new std::set<pid_t>;
    return *pgids;
}

void SweepProcessGroups() {
    std::lock_guard<std::mutex> lock(g_pgids_mu);
    for (const pid_t pid : Pgids()) {
        // killpg first (whole tree, valid once setsid ran -- always by now);
        // kill(pid) covers the fork-to-setsid window for anything pathological
        // enough to still be in it.
        killpg(pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    // No reaping: at exit, children become init's problem.
}

}  // namespace

void TrackPgid(pid_t pid) {
    // Registered on first use, after the first fork exists to clean up after.
    static const bool registered = [] {
        std::atexit(SweepProcessGroups);
        return true;
    }();
    (void)registered;
    std::lock_guard<std::mutex> lock(g_pgids_mu);
    Pgids().insert(pid);
}

void ForgetPgid(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_pgids_mu);
    Pgids().erase(pid);
}

void KillProcessGroup(pid_t pid) {
    // killpg reaches the whole tree once setsid() has run. kill() covers the
    // window before it: between fork() and setsid() the child's pgid is still
    // ours, so killpg(pid, ...) finds no such group, fails ESRCH, and would
    // leave a blocking waitpid below waiting for a process nothing killed.
    killpg(pid, SIGKILL);
    kill(pid, SIGKILL);
}

bool ReapAfterKill(pid_t pid) {
    KillProcessGroup(pid);
    int status = 0;
    // Bounded, not blocking: SIGKILL's "the process will die" does not hold
    // for a D-state child, and a blocking waitpid there would park the
    // caller (a session worker, at shutdown) forever. 5 s matches the bash
    // plugin's post-kill reap ceiling; an unreaped child is a zombie at
    // worst, never a hang.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPostKillReapMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) return true;
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return false;  // ECHILD: reaped elsewhere
        poll(nullptr, 0, 25);
    }
    return false;
}

std::string ResolveOnPath(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) return name;  // caller-resolved
    const char* path_env = ::getenv("PATH");
    if (path_env == nullptr || *path_env == '\0') return {};
    std::error_code ec;
    std::string_view path(path_env);
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = path.find(':', start);
        const auto dir =
            path.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                             : end - start);
        if (!dir.empty()) {
            const auto candidate = std::filesystem::path(dir) / name;
            if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

// --- Launch ------------------------------------------------------------------------------

Launched Launch(const LaunchSpec& spec) {
    Launched proc;

    // All pipe fds O_CLOEXEC so nothing leaks into grandchildren: a daemonising
    // process holding a read end open would otherwise keep the parent reading
    // until the deadline even after the direct child exited. The child re-points
    // exactly the fds the plan names.
    struct PipeFds {
        ScopedFd parent;
        ScopedFd child;
    };
    std::vector<PipeFds> fds(spec.pipes.size());
    for (std::size_t i = 0; i < spec.pipes.size(); ++i) {
        int pair[2];
        if (pipe(pair) != 0) {
            proc.spawn_error = std::string("pipe: ") + std::strerror(errno);
            return proc;
        }
        fcntl(pair[0], F_SETFD, fcntl(pair[0], F_GETFD) | FD_CLOEXEC);
        fcntl(pair[1], F_SETFD, fcntl(pair[1], F_GETFD) | FD_CLOEXEC);
        if (spec.pipes[i].child_end == PipePlan::ChildEnd::kRead) {
            fds[i].child = ScopedFd(pair[0]);
            fds[i].parent = ScopedFd(pair[1]);
        } else {
            fds[i].child = ScopedFd(pair[1]);
            fds[i].parent = ScopedFd(pair[0]);
        }
    }

    // /dev/null stdin when no plan covers fd 0. Opened in the parent so a
    // failure is reportable rather than a silent mystery in the child.
    ScopedFd devnull;
    const bool stdin_covered = [&] {
        for (const auto& plan : spec.pipes) {
            for (const auto& dup : plan.dups) {
                if (dup.child_fd == STDIN_FILENO) return true;
            }
        }
        return false;
    }();
    if (!stdin_covered) {
        devnull = ScopedFd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        if (devnull.get() < 0) {
            proc.spawn_error = std::string("open /dev/null: ") + std::strerror(errno);
            return proc;
        }
    }

    // Built in the parent BEFORE fork so the child only dereferences, never
    // allocates, after it. The sorted env map gives deterministic envp order.
    std::vector<std::string> env_strings;
    env_strings.reserve(spec.env.size());
    for (const auto& [key, value] : spec.env) env_strings.push_back(key + "=" + value);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> arg_copy = spec.argv;
    if (arg_copy.empty()) {
        proc.spawn_error = "empty argv";
        return proc;
    }
    const std::string exec_path = ResolveOnPath(arg_copy[0]);
    if (exec_path.empty()) {
        proc.spawn_error = "'" + arg_copy[0] + "' not found on PATH";
        return proc;
    }
    std::vector<char*> argvp;
    argvp.reserve(arg_copy.size() + 1);
    for (auto& s : arg_copy) argvp.push_back(s.data());
    argvp.push_back(nullptr);

    const std::string cwd_string = spec.cwd.string();
    const rlim_t cpu = spec.cpu_seconds > 0 ? static_cast<rlim_t>(spec.cpu_seconds)
                                            : RLIM_INFINITY;
    const rlim_t fsize = static_cast<rlim_t>(spec.file_size_bytes);

    const pid_t pid = fork();
    if (pid < 0) {
        proc.spawn_error = std::string("fork: ") + std::strerror(errno);
        return proc;
    }

    if (pid == 0) {
        // Child. Plain C only from here: no C++ runtime, no exceptions, every
        // failure is _exit.
        setsid();  // Own session + process group (pgid == pid): killpg reaches
                   // the whole tree, since group membership is inherited by fork.

        // Length comes from the literal's type: strlen is avoided along with
        // strerror, since this is a post-fork child of a multithreaded process
        // and only async-signal-safe calls are sound here.
        auto child_die = []<std::size_t N>(const char (&message)[N]) -> int {
            (void)!write(STDERR_FILENO, message, N - 1);
            (void)!write(STDERR_FILENO, "\n", 1);
            return 127;
        };

        // RLIMIT_CPU is a backstop for CPU-bound runaways; the wall-clock
        // deadline normally fires first. The grace keeps the two from racing on
        // a command that legitimately spends its budget. Checked, like
        // hook_shell.cpp's identical limits: an inherited hard rlimit already
        // below the requested value makes setrlimit fail (EPERM) rather than
        // clamp, and silently ignoring that would run the child with no
        // CPU/file-size backstop at all -- defeating the point of setting one.
        rlimit cpu_limit{};
        cpu_limit.rlim_cur = cpu_limit.rlim_max = cpu;
        if (setrlimit(RLIMIT_CPU, &cpu_limit) != 0) _exit(child_die("setrlimit(RLIMIT_CPU) failed"));

        rlimit fsize_limit{};
        fsize_limit.rlim_cur = fsize_limit.rlim_max = fsize;
        if (setrlimit(RLIMIT_FSIZE, &fsize_limit) != 0) {
            _exit(child_die("setrlimit(RLIMIT_FSIZE) failed"));
        }

        for (std::size_t i = 0; i < spec.pipes.size(); ++i) {
            // fds[i].child is exactly the end the plan named; dup2 clears
            // FD_CLOEXEC on the copy, which is what 0/1/2 want. A passthrough
            // fd > 2 needs the explicit clear below or exec closes it.
            const int source = fds[i].child.get();
            for (const auto& dup : spec.pipes[i].dups) {
                if (dup2(source, dup.child_fd) < 0) _exit(child_die("dup2 failed"));
                if (dup.clear_cloexec) fcntl(dup.child_fd, F_SETFD, 0);
            }
        }
        if (!stdin_covered) dup2(devnull.get(), STDIN_FILENO);

        if (chdir(cwd_string.c_str()) != 0) _exit(child_die("chdir to session cwd failed"));

        execve(exec_path.c_str(), argvp.data(), envp.data());

        // execve returns only on failure.
        if (errno == ENOENT) _exit(child_die("program not found"));
        if (errno == EACCES) _exit(child_die("program not executable"));
        _exit(child_die("failed to exec program"));
    }

    // Parent. The ScopedFd resets and release() calls cannot throw, but
    // TrackPgid can (a std::set insert, plus the first-use atexit
    // registration) -- and a throw past this point abandons a forked, live
    // child with its pgid registered in the sweep set. Contained: kill the
    // group and reap before propagating, so no caller ever inherits a
    // Launched that is half-built or a child nobody owns.
    try {
        for (auto& f : fds) f.child.reset();  // the child's ends are the child's now

        proc.ok = true;
        proc.pid = pid;
        proc.parent_fds.reserve(fds.size());
        for (auto& f : fds) proc.parent_fds.push_back(ScopedFd(f.parent.release()));
        TrackPgid(pid);
    } catch (...) {
        // Syscalls, not the RAII helpers: nothing here may throw from inside
        // a catch that is already cleaning up after a throw.
        ::killpg(pid, SIGKILL);
        ::kill(pid, SIGKILL);
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kPostKillReapMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t rc = waitpid(pid, &status, WNOHANG);
            if (rc != 0) break;  // reaped, or ECHILD
            poll(nullptr, 0, 25);
        }
        ForgetPgid(pid);
        throw;
    }
    return proc;
}

}  // namespace minimal::sidecar
