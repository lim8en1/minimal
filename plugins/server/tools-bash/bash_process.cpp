#include "bash_process.hpp"

#include <fcntl.h>
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
#include <utility>
#include <vector>

namespace minimal::tools_bash {

// --- CappedBuffer ------------------------------------------------------------------

void CappedBuffer::Append(std::string_view chunk) {
    if (head_.size() < head_cap_) {
        const auto take = std::min(head_cap_ - head_.size(), chunk.size());
        head_.append(chunk.data(), take);
        chunk.remove_prefix(take);
    }
    tail_.append(chunk.data(), chunk.size());

    // AMORTIZED, not per-chunk. Erasing down to exactly tail_cap_ on every
    // append means reads arriving in 8 KiB chunks memmove ~tail_cap_ bytes
    // each once the tail is full: a 200 MB build log -- an entirely ordinary
    // `make` -- is ~19 GB of memmove on the calling thread, spent purely on
    // throwing bytes away. Letting the tail grow to 2x before compacting
    // makes the work proportional to the data instead: each compaction
    // discards at least tail_cap_ bytes, so the total moved is O(total
    // output) rather than O(total output x tail_cap_ / chunk). The peak cost
    // is one extra tail_cap_ of memory, the same order the buffer already
    // holds.
    if (tail_.size() > 2 * tail_cap_) Compact();
}

std::string CappedBuffer::Render() {
    // Render is the only reader, and it needs the tail at its true size --
    // Append deliberately leaves it over-long between compactions.
    Compact();
    if (dropped_ == 0) return head_ + tail_;
    return head_ + "\n[" + std::to_string(dropped_) + " bytes omitted]\n" + tail_;
}

void CappedBuffer::Compact() {
    if (tail_.size() <= tail_cap_) return;
    dropped_ += tail_.size() - tail_cap_;
    tail_.erase(0, tail_.size() - tail_cap_);
}

// --- The state trailer -------------------------------------------------------------

std::string WrapCommand(const std::string& command) {
    // A command ending in a backslash is a line continuation: backslash-newline
    // glues the next physical line onto it, so the plain separator below made
    // `ls \` run `ls __minimal_rc=$?` -- the marker became an argument of the
    // user's command, and the state the trailer exists to report was lost. An
    // EXTRA newline first is correct for both parities: an odd trailing
    // backslash count consumes the first newline as continuation and the second
    // ends the (now empty) logical line; an even count is a literal backslash,
    // the first newline ends the command, and the second is a no-op empty line.
    const bool trailing_backslash = !command.empty() && command.back() == '\\';
    return command + (trailing_backslash ? "\n\n" : "\n") +
           "__minimal_rc=$?\n{ pwd -P; printf '\\0'; env -0; } >&" +
           std::to_string(kStateFd) + "\nexit $__minimal_rc\n";
}

ShellState ParseStatePayload(const std::string& payload) {
    ShellState state;
    const auto nul = payload.find('\0');
    if (nul == std::string::npos || nul == 0) return state;

    // `pwd` terminates its line, so the cwd arrives with a trailing newline that
    // would make every subsequent chdir() fail.
    std::string cwd = payload.substr(0, nul);
    while (!cwd.empty() && (cwd.back() == '\n' || cwd.back() == '\r')) cwd.pop_back();
    if (cwd.empty() || cwd.front() != '/') return state;  // must be an absolute path
    state.cwd = cwd;

    std::size_t pos = nul + 1;
    while (pos < payload.size()) {
        const auto end = payload.find('\0', pos);
        const auto entry = payload.substr(pos, end == std::string::npos ? std::string::npos
                                                                         : end - pos);
        const auto eq = entry.find('=');
        if (eq != std::string::npos && eq != 0) {
            state.env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (state.env.empty()) return state;  // a shell always reports something
    state.ok = true;
    return state;
}

// --- Launch / kill -------------------------------------------------------------------

LaunchedProcess LaunchProcess(const std::string& wrapped, const SessionShellState& state,
                              const BashSettings& settings, std::int64_t timeout_ms) {
    LaunchedProcess proc;

    // O_CLOEXEC so these do not leak into grandchildren: a daemonising process
    // holding a read end open would otherwise keep the parent reading until the
    // deadline even after the direct child exited.
    int out_fds[2];
    int state_fds[2];
    if (pipe(out_fds) != 0) {
        proc.spawn_error = std::strerror(errno);
        return proc;
    }
    if (pipe(state_fds) != 0) {
        proc.spawn_error = std::strerror(errno);
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        return proc;
    }
    for (int fd : {out_fds[0], out_fds[1], state_fds[0], state_fds[1]}) {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    ScopedFd out_read(out_fds[0]), out_write(out_fds[1]);
    ScopedFd state_read(state_fds[0]), state_write(state_fds[1]);

    // Opened in the parent so a failure is reportable rather than a silent
    // mystery in the child. Without this the child inherits the server's stdin
    // and any command that reads fd 0 blocks until the deadline.
    ScopedFd devnull(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (devnull.get() < 0) {
        proc.spawn_error = std::string("open /dev/null: ") + std::strerror(errno);
        return proc;
    }

    // Built in the parent BEFORE fork so the child only dereferences, never
    // allocates, after it. The sorted env map gives deterministic envp order.
    std::vector<std::string> env_strings;
    env_strings.reserve(state.env.size());
    for (const auto& [key, value] : state.env) env_strings.push_back(key + "=" + value);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::string shell = settings.shell;
    std::string dash_c = "-c";
    std::string script = wrapped;
    char* argv_ptrs[] = {shell.data(), dash_c.data(), script.data(), nullptr};

    const std::string cwd = state.cwd.string();
    const rlim_t cpu_seconds = static_cast<rlim_t>(timeout_ms / 1000) + 5;
    const rlim_t file_bytes = static_cast<rlim_t>(settings.max_file_size_bytes);

    const pid_t pid = fork();
    if (pid < 0) {
        proc.spawn_error = std::strerror(errno);
        return proc;
    }

    if (pid == 0) {
        // Child. Plain C only from here: no C++ runtime, no exceptions, every
        // failure is _exit.
        setsid();  // Own session + process group (pgid == pid): killpg reaches
                   // the whole tree, since group membership is inherited by fork.

        // RLIMIT_CPU is a backstop for CPU-bound runaways; the wall-clock
        // deadline normally fires first. The grace keeps the two from racing on
        // a command that legitimately spends its budget.
        rlimit cpu{};
        cpu.rlim_cur = cpu.rlim_max = cpu_seconds;
        setrlimit(RLIMIT_CPU, &cpu);

        rlimit fsize{};
        fsize.rlim_cur = fsize.rlim_max = file_bytes;
        setrlimit(RLIMIT_FSIZE, &fsize);

        // Length comes from the literal's type: strlen is avoided along with
        // strerror, since this is a post-fork child of a multithreaded process
        // and only async-signal-safe calls are sound here. stderr is already the
        // pipe, so the parent sees this in the command's output.
        auto child_die = []<std::size_t N>(const char (&message)[N]) -> int {
            (void)!write(STDERR_FILENO, message, N - 1);
            (void)!write(STDERR_FILENO, "\n", 1);
            return 127;
        };

        if (chdir(cwd.c_str()) != 0) _exit(child_die("chdir to session cwd failed"));

        // dup2 clears FD_CLOEXEC on the copy, which is what we want for 0/1/2 and
        // for the state fd -- the shell must be able to write to it.
        dup2(devnull.get(), STDIN_FILENO);
        dup2(out_write.get(), STDOUT_FILENO);
        dup2(out_write.get(), STDERR_FILENO);  // merged: one stream, in order
        dup2(state_write.get(), kStateFd);
        // dup2 is a no-op when the fd is already in place, leaving FD_CLOEXEC set
        // from the pipe() setup above -- which would close the state channel on
        // exec and silently lose every cd/export.
        fcntl(kStateFd, F_SETFD, 0);

        execve(shell.c_str(), argv_ptrs, envp.data());

        // execve returns only on failure.
        if (errno == ENOENT) _exit(child_die("shell not found"));
        if (errno == EACCES) _exit(child_die("shell not executable"));
        _exit(child_die("failed to exec shell"));
    }

    // Parent.
    out_write.reset();
    state_write.reset();
    devnull.reset();

    proc.ok = true;
    proc.pid = pid;
    proc.out_fd = out_read.release();
    proc.state_fd = state_read.release();
    TrackPgid(pid);
    return proc;
}

// --- The pgid set and the atexit sweep ------------------------------------------------

namespace {

std::mutex g_pgids_mu;
std::set<pid_t>& Pgids() {
    static std::set<pid_t> pgids;
    return pgids;
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

}  // namespace minimal::tools_bash
