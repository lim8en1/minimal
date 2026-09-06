#include "python_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

namespace minimal::tools_python {
namespace {

// The drain/reap slices. The interpreter's reader uses its own loop; these
// bound RunCaptured's wait, whose deadline is the kill signal.
constexpr int kWaitSliceMs = 50;

// Longest protocol/diag line RunCaptured's merged capture is involved in --
// it captures everything, so this only bounds memory, not behavior.
void AppendCapped(std::string& head, std::string& tail, std::size_t& dropped,
                  std::size_t head_cap, std::size_t tail_cap, std::string_view chunk) {
    if (head.size() < head_cap) {
        const auto take = std::min(head_cap - head.size(), chunk.size());
        head.append(chunk.data(), take);
        chunk.remove_prefix(take);
    }
    tail.append(chunk.data(), chunk.size());
    if (tail.size() > 2 * tail_cap) {
        dropped += tail.size() - tail_cap;
        tail.erase(0, tail.size() - tail_cap);
    }
}

std::string RenderCapped(const std::string& head, const std::string& tail, std::size_t dropped) {
    if (dropped == 0) return head + tail;
    return head + "\n[" + std::to_string(dropped) + " bytes omitted]\n" + tail;
}

std::mutex g_pgids_mu;
std::set<pid_t>& Pgids() {
    static std::set<pid_t> pgids;
    return pgids;
}

void SweepProcessGroups() {
    std::lock_guard<std::mutex> lock(g_pgids_mu);
    for (const pid_t pid : Pgids()) {
        // killpg first (whole tree, valid once setsid ran -- always by now);
        // kill(pid) covers the fork-to-setsid window for anything still in it.
        killpg(pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    // No reaping: at exit, children become init's problem.
}

}  // namespace

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

LaunchedInterpreter LaunchInterpreter(const std::vector<std::string>& argv,
                                      const std::filesystem::path& cwd,
                                      const std::map<std::string, std::string>& env,
                                      std::int64_t cpu_seconds, std::int64_t file_size_bytes) {
    LaunchedInterpreter proc;

    // O_CLOEXEC everywhere: nothing leaks into grandchildren except the fds we
    // deliberately re-point (0/1/2/3 below).
    int in_fds[2];
    int out_fds[2];
    int diag_fds[2];
    if (pipe(in_fds) != 0) {
        proc.spawn_error = std::string("pipe: ") + std::strerror(errno);
        return proc;
    }
    if (pipe(out_fds) != 0) {
        proc.spawn_error = std::string("pipe: ") + std::strerror(errno);
        ::close(in_fds[0]);
        ::close(in_fds[1]);
        return proc;
    }
    if (pipe(diag_fds) != 0) {
        proc.spawn_error = std::string("pipe: ") + std::strerror(errno);
        ::close(in_fds[0]);
        ::close(in_fds[1]);
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        return proc;
    }
    for (int fd : {in_fds[0], in_fds[1], out_fds[0], out_fds[1], diag_fds[0], diag_fds[1]}) {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    ScopedFd in_read(in_fds[0]), in_write(in_fds[1]);
    ScopedFd out_read(out_fds[0]), out_write(out_fds[1]);
    ScopedFd diag_read(diag_fds[0]), diag_write(diag_fds[1]);

    // Built in the parent BEFORE fork so the child only dereferences, never
    // allocates, after it. Sorted map gives deterministic envp order.
    std::vector<std::string> env_strings;
    env_strings.reserve(env.size());
    for (const auto& [key, value] : env) env_strings.push_back(key + "=" + value);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> arg_copy = argv;
    std::vector<char*> argvp;
    argvp.reserve(arg_copy.size() + 1);
    for (auto& s : arg_copy) argvp.push_back(s.data());
    argvp.push_back(nullptr);

    const std::string exec_path = ResolveOnPath(arg_copy[0]);
    if (exec_path.empty()) {
        proc.spawn_error = "interpreter '" + arg_copy[0] + "' not found on PATH";
        return proc;
    }
    const std::string cwd_string = cwd.string();
    const rlim_t cpu = cpu_seconds > 0 ? static_cast<rlim_t>(cpu_seconds) : RLIM_INFINITY;
    const rlim_t fsize = static_cast<rlim_t>(file_size_bytes);

    const pid_t pid = fork();
    if (pid < 0) {
        proc.spawn_error = std::string("fork: ") + std::strerror(errno);
        return proc;
    }

    if (pid == 0) {
        // Child. Plain C only from here: no C++ runtime, no exceptions, every
        // failure is _exit.
        setsid();  // Own session + process group (pgid == pid).

        rlimit cpu_limit{};
        cpu_limit.rlim_cur = cpu_limit.rlim_max = cpu;
        setrlimit(RLIMIT_CPU, &cpu_limit);

        rlimit fsize_limit{};
        fsize_limit.rlim_cur = fsize_limit.rlim_max = fsize;
        setrlimit(RLIMIT_FSIZE, &fsize_limit);

        auto child_die = []<std::size_t N>(const char (&message)[N]) -> int {
            (void)!write(STDERR_FILENO, message, N - 1);
            (void)!write(STDERR_FILENO, "\n", 1);
            return 127;
        };

        // The REAL stdout+stderr go to the diagnostics pipe; the protocol
        // stdout rides fd 3 (FD_CLOEXEC cleared) and protocol stdin stays on
        // fd 0. Executed code writing to fd 1 therefore cannot corrupt the
        // response stream -- it lands in diagnostics, which the parent drains
        // and only reads on death.
        dup2(in_read.get(), STDIN_FILENO);
        dup2(diag_write.get(), STDOUT_FILENO);
        dup2(diag_write.get(), STDERR_FILENO);
        fcntl(kProtoStdoutFd, F_SETFD, 0);  // survives exec
        // kProtoStdoutFd may collide with one of the pipe fds; dup2 to itself
        // is a no-op, but only AFTER the CLOEXEC clear above is meaningful.
        dup2(out_write.get(), kProtoStdoutFd);

        if (chdir(cwd_string.c_str()) != 0) _exit(child_die("chdir to project root failed"));

        execve(exec_path.c_str(), argvp.data(), envp.data());

        // execve returns only on failure.
        if (errno == ENOENT) _exit(child_die("interpreter not found"));
        if (errno == EACCES) _exit(child_die("interpreter not executable"));
        _exit(child_die("failed to exec interpreter"));
    }

    // Parent. Contained construction, same as tools-bash: a throw past this
    // point must not abandon a live forked child.
    try {
        in_read.reset();
        out_write.reset();
        diag_write.reset();

        proc.ok = true;
        proc.pid = pid;
        proc.in_fd = in_write.release();
        proc.out_fd = out_read.release();
        proc.diag_fd = diag_read.release();
        TrackPgid(pid);
    } catch (...) {
        ::killpg(pid, SIGKILL);
        ::kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        ForgetPgid(pid);
        throw;
    }
    return proc;
}

CapturedRun RunCaptured(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::map<std::string, std::string>& env,
                        std::int64_t timeout_seconds, std::size_t max_output_bytes) {
    CapturedRun run;

    int out_fds[2];
    if (pipe(out_fds) != 0) {
        run.error = std::string("pipe: ") + std::strerror(errno);
        return run;
    }
    for (int fd : {out_fds[0], out_fds[1]}) {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    ScopedFd out_read(out_fds[0]), out_write(out_fds[1]);

    std::vector<std::string> env_strings;
    env_strings.reserve(env.size());
    for (const auto& [key, value] : env) env_strings.push_back(key + "=" + value);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<std::string> arg_copy = argv;
    std::vector<char*> argvp;
    argvp.reserve(arg_copy.size() + 1);
    for (auto& s : arg_copy) argvp.push_back(s.data());
    argvp.push_back(nullptr);

    const std::string exec_path = ResolveOnPath(arg_copy[0]);
    if (exec_path.empty()) {
        run.error = "'" + arg_copy[0] + "' not found on PATH";
        return run;
    }
    const std::string cwd_string = cwd.string();
    // RLIMIT_CPU backstop: the wall-clock deadline normally fires first; the
    // grace keeps the two from racing on a slow-but-healthy install.
    const rlim_t cpu = static_cast<rlim_t>(timeout_seconds) + 30;

    const pid_t pid = fork();
    if (pid < 0) {
        run.error = std::string("fork: ") + std::strerror(errno);
        return run;
    }
    if (pid == 0) {
        setsid();
        rlimit cpu_limit{};
        cpu_limit.rlim_cur = cpu_limit.rlim_max = cpu;
        setrlimit(RLIMIT_CPU, &cpu_limit);
        rlimit fsize_limit{};
        fsize_limit.rlim_cur = fsize_limit.rlim_max = 1024LL * 1024 * 1024;
        setrlimit(RLIMIT_FSIZE, &fsize_limit);
        dup2(out_write.get(), STDOUT_FILENO);
        dup2(out_write.get(), STDERR_FILENO);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        if (chdir(cwd_string.c_str()) != 0) _exit(127);
        execve(exec_path.c_str(), argvp.data(), envp.data());
        _exit(127);
    }

    out_write.reset();
    TrackPgid(pid);

    std::string head, tail;
    std::size_t dropped = 0;
    const std::size_t head_cap = max_output_bytes / 4;
    const std::size_t tail_cap = max_output_bytes - head_cap;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max<std::int64_t>(1, timeout_seconds));
    bool io_open = true;
    bool timed_out = false;
    while (io_open) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        pollfd fds[1];
        fds[0] = pollfd{out_read.get(), POLLIN, 0};
        const int slice = kWaitSliceMs;
        const int rc = poll(fds, 1, slice);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;
        char chunk[8192];
        const ssize_t n = read(out_read.get(), chunk, sizeof(chunk));
        if (n > 0) {
            AppendCapped(head, tail, dropped, head_cap, tail_cap,
                         std::string_view(chunk, static_cast<std::size_t>(n)));
        } else if (n == 0 || errno != EINTR) {
            io_open = false;
        }
    }

    int status = 0;
    bool reaped = false;
    if (timed_out) {
        KillProcessGroup(pid);
    }
    while (true) {
        const pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            reaped = true;
            break;
        }
        if (rc < 0) break;  // ECHILD: already reaped elsewhere; status unusable
        if (std::chrono::steady_clock::now() >= deadline + std::chrono::seconds(5)) {
            KillProcessGroup(pid);  // belt and braces; SIGKILL bounds the wait
        }
        poll(nullptr, 0, kWaitSliceMs);
    }
    if (reaped) ForgetPgid(pid);
    if (!reaped) {
        ReapAfterKill(pid);
        ForgetPgid(pid);
    }

    run.ok = reaped && !timed_out;
    run.output = RenderCapped(head, tail, dropped);
    if (timed_out) {
        run.exit_status = -1;
        run.error = "timed out after " + std::to_string(timeout_seconds) + "s; process group killed";
    } else if (reaped) {
        run.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return run;
}

void ReapAfterKill(pid_t pid) {
    KillProcessGroup(pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void TrackPgid(pid_t pid) {
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
    // killpg reaches the whole tree once setsid() has run; kill() covers the
    // fork-to-setsid window, where killpg would find no such group.
    killpg(pid, SIGKILL);
    kill(pid, SIGKILL);
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

}  // namespace minimal::tools_python
