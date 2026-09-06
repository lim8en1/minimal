#include "minimal/hooks/hook_shell.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

namespace minimal::hooks {
namespace {

// Bounds what a hook's output may hold in memory. Only the first non-empty
// line ever becomes a block reason; the cap exists so a hook piping a build
// log cannot grow a string without limit on the caller's thread. Past the cap
// the parent simply stops reading -- the child blocks on write and the
// deadline (or its own exit) reaps it.
constexpr std::size_t kMaxCapturedBytes = 1 << 20;  // 1 MiB
constexpr std::size_t kMaxBlockReasonBytes = 500;
// Matches the bash tool's default filesize backstop (bash_settings.hpp): a
// hook script redirecting to a file is bounded the same way its model-driven
// cousin is.
constexpr rlim_t kMaxHookFileSizeBytes = 1024LL * 1024 * 1024;  // 1 GiB

class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

std::string FirstLine(std::string_view captured) {
    std::string_view line;
    std::size_t start = 0;
    while (start < captured.size()) {
        const auto end = captured.find('\n', start);
        std::string_view candidate =
            captured.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                 : end - start);
        // Trim a trailing CR and surrounding whitespace.
        while (!candidate.empty() &&
               (candidate.back() == '\r' || candidate.back() == ' ' || candidate.back() == '\t')) {
            candidate.remove_suffix(1);
        }
        while (!candidate.empty() && (candidate.front() == ' ' || candidate.front() == '\t')) {
            candidate.remove_prefix(1);
        }
        if (!candidate.empty()) {
            line = candidate;
            break;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (line.size() > kMaxBlockReasonBytes) line = line.substr(0, kMaxBlockReasonBytes);
    return std::string(line);
}

}  // namespace

ShellActionResult RunShellAction(const std::string& command, std::string_view stdin_payload,
                                 std::chrono::milliseconds timeout,
                                 const std::function<bool()>& is_cancelled) {
    ShellActionResult result;

    // O_CLOEXEC so neither end leaks into grandchildren. Both arrays start at
    // -1: POSIX does not define the contents of an fd array when pipe()
    // fails, so the cleanup below must read initialized values or it can
    // close() a garbage number that happens to be another session's live fd.
    int in_fds[2] = {-1, -1};
    int out_fds[2] = {-1, -1};
    if (pipe(in_fds) != 0 || pipe(out_fds) != 0) {
        result.outcome = ShellActionOutcome::kFailOpen;
        result.fail_reason = std::string("pipe: ") + std::strerror(errno);
        for (int fd : in_fds) ::close(fd);
        for (int fd : out_fds) ::close(fd);
        return result;
    }
    for (int fd : {in_fds[0], in_fds[1], out_fds[0], out_fds[1]}) {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    ScopedFd in_read(in_fds[0]), in_write(in_fds[1]);
    ScopedFd out_read(out_fds[0]), out_write(out_fds[1]);

    // The child's stdin is the payload pipe, NOT the server's: a hook that
    // reads fd 0 reads the JSON payload and then EOF, never the server's
    // terminal or a mystery fd.
    std::string shell = "/bin/sh";
    std::string dash_c = "-c";
    std::string script = command;
    char* argv_ptrs[] = {shell.data(), dash_c.data(), script.data(), nullptr};

    // A zero timeout means "no budget", which a CANCELLABLE sync emission may
    // ask for -- its caller can still abandon the wait. The async path passes
    // a null canceller, though, and the hook worker is a single thread: zero
    // budget plus no cancellation means one wedged script blocks every later
    // hook, forever. Uncancellable runs therefore get a floor budget, which is
    // also what the child's RLIMIT_CPU backstop below is floored with.
    static constexpr std::chrono::milliseconds kUncancellableFloor{60000};
    const bool cancellable = is_cancelled != nullptr;
    const std::chrono::milliseconds effective =
        timeout.count() > 0 ? timeout : (cancellable ? std::chrono::milliseconds::zero()
                                                     : kUncancellableFloor);

    const pid_t pid = fork();
    if (pid < 0) {
        result.outcome = ShellActionOutcome::kFailOpen;
        result.fail_reason = std::string("fork: ") + std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        // Child. Plain C only: post-fork child of a multithreaded process,
        // async-signal-safe calls only, every failure is _exit.
        setsid();  // pgid == pid: killpg reaches the whole tree.

        if (effective.count() > 0) {
            // CPU backstop behind the wall-clock deadline, like the bash tool.
            rlimit cpu{};
            cpu.rlim_cur = cpu.rlim_max = static_cast<rlim_t>(effective.count() / 1000) + 5;
            setrlimit(RLIMIT_CPU, &cpu);

            // Filesize backstop, matching the bash tool: a runaway script
            // redirecting to a file must not fill the disk unbounded.
            rlimit fsize{};
            fsize.rlim_cur = fsize.rlim_max = kMaxHookFileSizeBytes;
            setrlimit(RLIMIT_FSIZE, &fsize);
        }

        dup2(in_read.get(), STDIN_FILENO);
        dup2(out_write.get(), STDOUT_FILENO);
        dup2(out_write.get(), STDERR_FILENO);  // merged: one stream, in order

        execv(shell.c_str(), argv_ptrs);

        // execv returns only on failure.
        static const char kMessage[] = "minimal hook: failed to exec /bin/sh\n";
        (void)!write(STDERR_FILENO, kMessage, sizeof(kMessage) - 1);
        _exit(127);
    }

    // Parent.
    in_read.reset();
    out_write.reset();

    const bool budgeted = effective.count() > 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(effective.count());
    const auto expired = [&] {
        return budgeted && std::chrono::steady_clock::now() >= deadline;
    };
    const auto cancelled = [&] { return is_cancelled && is_cancelled(); };

    std::string captured;
    std::size_t written = 0;
    bool in_open = !stdin_payload.empty();
    bool out_open = true;
    bool abandoned = false;  // deadline or cancellation fired mid-flight

    while (in_open || out_open) {
        if (cancelled() || expired()) {
            abandoned = true;
            break;
        }

        pollfd fds[2];
        int nfds = 0;
        int in_slot = -1;
        if (in_open) {
            in_slot = nfds;
            fds[nfds++] = pollfd{in_write.get(), POLLOUT, 0};
        }
        const int out_slot = nfds;
        if (out_open) {
            fds[nfds++] = pollfd{out_read.get(), POLLIN, 0};
        }

        // Sliced so cancellation is noticed within 50 ms, not at the deadline.
        const int rc = poll(fds, static_cast<nfds_t>(nfds), 50);
        if (rc < 0) {
            if (errno == EINTR) continue;
            // Unexpected poll failure: stop writing, keep draining what we can.
            in_open = false;
            if (!out_open) {
                result.outcome = ShellActionOutcome::kFailOpen;
                result.fail_reason = std::string("poll: ") + std::strerror(errno);
                abandoned = true;
                break;
            }
        }
        if (rc == 0) continue;

        if (in_slot >= 0 && in_open) {
            if ((fds[in_slot].revents & POLLOUT) != 0) {
                const ssize_t n = write(in_write.get(), stdin_payload.data() + written,
                                        stdin_payload.size() - written);
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                    if (written >= stdin_payload.size()) {
                        // EOF on stdin: the payload is data, not a stream.
                        in_write.reset();
                        in_open = false;
                    }
                } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                    in_open = false;  // the child is gone or closed stdin; output tells the story
                }
            } else if ((fds[in_slot].revents & (POLLERR | POLLHUP)) != 0) {
                // The read end is gone -- the child exited without reading the
                // payload (the normal case for a notify-style hook) or closed
                // its stdin. Writing anyway was a one-way trip: with the
                // default SIGPIPE disposition it killed the whole server, and
                // a poll report of POLLERR|POLLHUP arrives exactly then. Stop
                // writing; the child's output tells the story.
                in_open = false;
            }
        }

        if (out_open && (fds[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            char chunk[8192];
            const ssize_t n = read(out_read.get(), chunk, sizeof(chunk));
            if (n > 0) {
                if (captured.size() < kMaxCapturedBytes) {
                    captured.append(chunk, static_cast<std::size_t>(
                                               std::min<std::size_t>(n, kMaxCapturedBytes -
                                                                                         captured.size())));
                }
                // Past the cap: deliberately NOT read-and-discard. The child
                // blocks on write and the deadline reaps it; spinning through
                // gigabytes just to count them buys nothing.
            } else if (n == 0 || errno != EINTR) {
                out_open = false;
            }
        }
    }

    // Reaping, deadline-bounded: EOF on the pipes does not mean the child is
    // gone. Same shape as the bash tool's reaper.
    int status = 0;
    bool reaped = false;
    while (!abandoned) {
        const pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            reaped = true;
            break;
        }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;  // ECHILD
        if (cancelled() || expired()) {
            abandoned = true;
            break;
        }
        poll(nullptr, 0, 10);
    }

    if (!reaped) {
        // kill(pid) covers the fork-to-setsid window where the child's pgid is
        // still ours and killpg would fail ESRCH.
        killpg(pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    if (abandoned) {
        result.outcome = ShellActionOutcome::kFailOpen;
        if (cancelled()) {
            result.fail_reason = "cancelled";
        } else {
            result.fail_reason = budgeted
                                     ? "timed out after " + std::to_string(effective.count()) +
                                           " ms"
                                     : "abandoned while reaping";
        }
        return result;
    }

    if (!WIFEXITED(status)) {
        result.outcome = ShellActionOutcome::kFailOpen;
        result.fail_reason = WIFSIGNALED(status)
                                 ? "terminated by signal " + std::to_string(WTERMSIG(status))
                                 : "did not exit normally";
        return result;
    }

    const int code = WEXITSTATUS(status);
    if (code == 0) {
        result.outcome = ShellActionOutcome::kPass;
        return result;
    }
    if (code == 2) {
        result.outcome = ShellActionOutcome::kBlock;
        result.block_reason = FirstLine(captured);
        if (result.block_reason.empty()) {
            result.block_reason = "blocked by hook (no reason given)";
        }
        return result;
    }

    result.outcome = ShellActionOutcome::kFailOpen;
    result.fail_reason = "exit status " + std::to_string(code);
    return result;
}

}  // namespace minimal::hooks
