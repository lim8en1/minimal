#include "bash_tool.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command_scanner.hpp"

namespace minimal::tools_bash {
namespace {

using tools::GrantOffer;
using tools::PermissionDisclosure;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

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

// --- Capped output buffer ------------------------------------------------------
//
// Head (1/4 of the cap) + tail (3/4): in a long build log both the first error
// and the final status matter. This bounds only what is PERSISTED into the
// ToolResultBlock; live streaming through emit_output is uncapped because that
// channel is lossy by design -- the session's outbound queue is the backpressure
// boundary, and the authoritative record is the capped result block here (the
// running-tools panel's tail behavior is a track-E client concern; see
// ARCHITECTURE.md "Running-tools panel").
class CappedBuffer {
public:
    explicit CappedBuffer(std::int64_t cap)
        : head_cap_(static_cast<std::size_t>(cap / 4)),
          tail_cap_(static_cast<std::size_t>(cap - cap / 4)) {}

    void Append(std::string_view chunk) {
        if (head_.size() < head_cap_) {
            const auto take = std::min(head_cap_ - head_.size(), chunk.size());
            head_.append(chunk.data(), take);
            chunk.remove_prefix(take);
        }
        tail_.append(chunk.data(), chunk.size());

        // AMORTIZED, not per-chunk. This used to erase down to exactly tail_cap_
        // on every append, and reads arrive in 8 KiB chunks: once the tail was
        // full, every single chunk memmoved ~tail_cap_ bytes to discard 8 KiB.
        // A 200 MB build log -- an entirely ordinary `make` -- meant ~25,000
        // erases of ~768 KB each, about 19 GB of memmove on the session worker,
        // spent purely on throwing bytes away.
        //
        // Letting the tail grow to 2x before compacting makes the work
        // proportional to the data instead: each compaction discards at least
        // tail_cap_ bytes, so the total moved is O(total output) rather than
        // O(total output x tail_cap_ / chunk). The peak cost is one extra
        // tail_cap_ of memory, which is the same order the buffer already holds.
        if (tail_.size() > 2 * tail_cap_) Compact();
    }

    std::string Render() const {
        // Render is the only reader, and it needs the tail at its true size --
        // Append deliberately leaves it over-long between compactions.
        const_cast<CappedBuffer*>(this)->Compact();
        if (dropped_ == 0) return head_ + tail_;
        return head_ + "\n[" + std::to_string(dropped_) + " bytes omitted]\n" + tail_;
    }

private:
    void Compact() {
        if (tail_.size() <= tail_cap_) return;
        dropped_ += tail_.size() - tail_cap_;
        tail_.erase(0, tail_.size() - tail_cap_);
    }

    std::size_t head_cap_, tail_cap_;
    std::string head_, tail_;
    std::size_t dropped_ = 0;
};

// --- The state trailer ----------------------------------------------------------
//
// `cd` and `export` inside `sh -c` die with the subshell, so the shell reports
// its final cwd and environment on a side channel and we adopt them. This is what
// lets the process stay per-call (no long-lived interpreter to corrupt, hang, or
// leak) while `cd`/`export` still chain across calls.
//
// `pwd` and `printf` are shell builtins; `env` is not, so a command that removes
// it from PATH makes the trailer produce no environment. Adoption is then
// refused wholesale (see ParseStatePayload) and the previous state survives --
// which is also self-limiting, since the PATH change that broke it is discarded
// along with everything else rather than persisting.
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

struct ShellState {
    std::filesystem::path cwd;
    std::map<std::string, std::string> env;
    bool ok = false;
};

// Payload: <absolute cwd> NUL (KEY=VALUE NUL)*
//
// Validated rather than trusted. A command may write to fd 9 itself -- by
// accident or otherwise -- and session state must not be corruptible that way.
// Anything unexpected leaves the previous state untouched, which is also what a
// command that exits before the trailer runs produces (an empty payload).
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

// --- Execution -------------------------------------------------------------------
//
// fork + execve rather than posix_spawn: the rlimits must be set in the child
// before exec, and setrlimit is not on POSIX's async-signal-safe list, which is
// all a posix_spawn pre-exec function may call.

struct RunOutcome {
    bool spawn_failed = false;
    std::string spawn_error;
    bool io_error = false;
    std::string io_error_message;
    bool timed_out = false;
    bool cancelled = false;
    int wait_status = 0;   // valid unless spawn_failed
    std::string output;    // the capped, rendered stdout+stderr captured so far
    std::string state_payload;
    bool state_overflow = false;  // fd-9 payload exceeded the cap; state untouched
};

class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { reset(); }
    ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        reset();
        fd_ = std::exchange(other.fd_, -1);
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

RunOutcome RunProcess(const std::string& wrapped, const SessionShellState& state,
                      const BashSettings& settings, std::int64_t timeout_ms, ToolContext& ctx) {
    RunOutcome outcome;

    // O_CLOEXEC so these do not leak into grandchildren: a daemonising process
    // holding the read end open would otherwise keep the parent reading until the
    // deadline even after the direct child exited.
    int out_fds[2];
    int state_fds[2];
    if (pipe(out_fds) != 0) {
        outcome.spawn_failed = true;
        outcome.spawn_error = std::strerror(errno);
        return outcome;
    }
    if (pipe(state_fds) != 0) {
        outcome.spawn_failed = true;
        outcome.spawn_error = std::strerror(errno);
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        return outcome;
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
        outcome.spawn_failed = true;
        outcome.spawn_error = std::string("open /dev/null: ") + std::strerror(errno);
        return outcome;
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
        outcome.spawn_failed = true;
        outcome.spawn_error = std::strerror(errno);
        return outcome;
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    CappedBuffer buffer(settings.max_output_bytes);
    std::string state_payload;
    bool out_open = true, state_open = true;

    const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
    const auto cancelled = [&] { return ctx.is_cancelled && ctx.is_cancelled(); };

    while (out_open || state_open) {
        if (cancelled()) {
            outcome.cancelled = true;
            break;
        }
        if (expired()) {
            outcome.timed_out = true;
            break;
        }

        pollfd fds[2];
        int nfds = 0;
        const int out_slot = out_open ? nfds++ : -1;
        if (out_open) fds[out_slot] = pollfd{out_read.get(), POLLIN, 0};
        const int state_slot = state_open ? nfds++ : -1;
        if (state_open) fds[state_slot] = pollfd{state_read.get(), POLLIN, 0};

        // Sliced so cancellation is noticed within 50 ms, not at the deadline.
        const int rc = poll(fds, static_cast<nfds_t>(nfds), 50);
        if (rc < 0) {
            if (errno == EINTR) continue;
            outcome.io_error = true;
            outcome.io_error_message = std::strerror(errno);
            break;
        }
        if (rc == 0) continue;

        char chunk[8192];
        if (out_slot >= 0 && (fds[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(out_read.get(), chunk, sizeof(chunk));
            if (n > 0) {
                const std::string_view view(chunk, static_cast<std::size_t>(n));
                buffer.Append(view);
                if (ctx.emit_output) ctx.emit_output(view);
            } else if (n == 0 || errno != EINTR) {
                out_open = false;
            }
        }
        if (state_slot >= 0 && (fds[state_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(state_read.get(), chunk, sizeof(chunk));
            if (n > 0) {
                if (!outcome.state_overflow &&
                    state_payload.size() + static_cast<std::size_t>(n) <= kMaxStatePayloadBytes) {
                    state_payload.append(chunk, static_cast<std::size_t>(n));
                } else if (!outcome.state_overflow) {
                    outcome.state_overflow = true;
                    // Cleared AND latched: without the flag check on the next append the
        // payload simply started re-accumulating, so `yes >&9` refilled and
        // cleared 1 MiB over and over for the whole timeout. The payload is
        // discarded wholesale once overflow is set, so there is nothing to keep.
        state_payload.clear();
        state_payload.shrink_to_fit();  // discard wholesale: never adopt a prefix
                }
            } else if (n == 0 || errno != EINTR) {
                state_open = false;
            }
        }
    }

    // Reaping, deadline-bounded. EOF on the pipes does NOT mean the child is
    // gone -- it can close its stdio and keep running -- so an unconditional
    // blocking waitpid here would park this thread for the command's natural
    // life, well past timeout_ms.
    int status = 0;
    bool reaped = false;
    while (!outcome.timed_out && !outcome.cancelled) {
        const pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            reaped = true;
            break;
        }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;  // ECHILD: nothing left to reap
        if (cancelled()) {
            outcome.cancelled = true;
            break;
        }
        if (expired()) {
            outcome.timed_out = true;
            break;
        }
        poll(nullptr, 0, 10);  // brief sleep; no fd to wait on
    }

    if (!reaped) {
        // killpg reaches the whole tree once setsid() has run. kill() covers the
        // window before it: between fork() and setsid() the child's pgid is still
        // ours, so killpg(pid, ...) finds no such group, fails ESRCH, and would
        // leave the blocking waitpid below waiting for a process nothing killed.
        killpg(pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    outcome.wait_status = status;
    outcome.output = buffer.Render();
    outcome.state_payload = std::move(state_payload);
    return outcome;
}

// --- Audit log ---------------------------------------------------------------------

std::int64_t ElapsedMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

// Resolves a command word against the SESSION's PATH, so `export PATH=...` in an
// earlier call changes what a later one names. Display and grant keys use the
// result: `cp /bin/bash /tmp/git` then `git ...` discloses /tmp/git, not `git`.
// That does not *prevent* anything -- nothing at this layer does -- it makes it
// visible at the moment consent is given, which is the only place it can be.
std::optional<std::filesystem::path> ResolveOnPath(const SessionShellState& state,
                                                    const std::string& word) {
    std::error_code ec;
    if (word.find('/') != std::string::npos) {
        std::filesystem::path p = state.cwd;
        p /= word;  // absolute `word` replaces, relative appends
        const auto canonical = std::filesystem::canonical(p, ec);
        // is_regular_file, same as the PATH branch below -- the two branches
        // had drifted, and without it `access(dir, X_OK)` succeeds for a
        // DIRECTORY, offering a grant for a path that cannot be a program.
        // Over-discloses only, but disclosure is what consent is given against.
        if (ec) return std::nullopt;
        const bool regular = std::filesystem::is_regular_file(canonical, ec);
        if (!regular || ec || access(canonical.c_str(), X_OK) != 0) return std::nullopt;
        return canonical;
    }

    const auto path_it = state.env.find("PATH");
    if (path_it == state.env.end()) return std::nullopt;

    std::string_view dirs = path_it->second;
    for (;;) {
        const auto colon = dirs.find(':');
        const std::string_view dir = dirs.substr(0, colon);
        // An empty element means the current directory, like in a shell.
        const auto candidate = dir.empty() ? state.cwd / word : std::filesystem::path(dir) / word;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec &&
            access(candidate.c_str(), X_OK) == 0) {
            // Canonical so the grant key is stable: /var/... and /private/var/...
            // are the same binary, and must not need granting twice.
            const auto canonical = std::filesystem::canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
        // The check precedes the break so the LAST element is searched too.
        if (colon == std::string_view::npos) break;
        dirs.remove_prefix(colon + 1);
    }
    return std::nullopt;
}

// --- The tool ------------------------------------------------------------------------

class BashTool : public tools::Tool {
public:
    BashTool(BashSettings settings, BashSessionStateStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "bash";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Run a shell command. Pipes, redirection, &&, ||, ;, globs and $VAR all work -- "
            "prefer piping to a filter (`| tail -50`, `| grep -i error`) over returning a huge "
            "log, since everything returned enters the context. The working directory and "
            "environment PERSIST across calls: use `cd` and `export` normally, and pass "
            "restart=true to reset both. stdout and stderr are returned together; very long "
            "output is truncated in the middle; a non-zero exit status is an error.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}, {"description", "The shell command to run"}}},
              {"timeout_ms",
               {{"type", "integer"},
                {"description", "Wall-clock limit; defaults to " +
                                    std::to_string(settings_.timeout_ms) + ", capped at " +
                                    std::to_string(settings_.max_timeout_ms)}}},
              {"restart",
               {{"type", "boolean"},
                {"description", "Reset cwd and environment to their defaults before running"}}}}},
            {"required", nlohmann::json::array({"command"})}};
    }

    const ToolSpec& spec() const override { return spec_; }

    // Bash keeps the interface default for path confinement: there is no sound
    // way to know which paths an arbitrary command touches.
    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const ToolContext& ctx) const override {
        PermissionDisclosure disclosure;
        if (!input.contains("command") || !input.at("command").is_string()) {
            disclosure.summary = "bash: <invalid input>";
            disclosure.complete = false;
            return disclosure;
        }
        const auto command = input.at("command").get<std::string>();
        disclosure.summary = "bash: " + command;

        const auto state = store_.Snapshot(ctx.session_id, ctx.project_root);
        const auto scan = ScanCommand(command);
        disclosure.complete = scan.complete;

        for (const auto& word : scan.command_words) {
            const auto resolved = ResolveOnPath(state, word);
            // A word that resolves to nothing -- a builtin like `export`, or a
            // typo that will fail at exec -- has no binary to grant, so it
            // contributes no offer.
            //
            // No builtin table, deliberately. It would have to track the
            // *configured* shell's builtin set to be right, and getting it wrong
            // in the permissive direction is the dangerous half. Resolution
            // instead errs the safe way: macOS ships /usr/bin/cd, so `cd sub &&
            // ls` offers a grant for a binary the shell will never actually
            // exec. That over-discloses, which only ever makes the conjunction
            // stricter -- more keys to hold, never fewer.
            if (!resolved) continue;
            const auto path = resolved->string();
            disclosure.grants.push_back(GrantOffer{"bash:" + path, path + " *"});
        }
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        const auto started = std::chrono::steady_clock::now();

        if (!input.contains("command") || !input.at("command").is_string()) {
            return Reject(ctx, started, "missing required string field: command", "<invalid>");
        }
        const auto command = input.at("command").get<std::string>();

        // Checked rather than value(): a mistyped flag is a clear error, not an
        // exception that would unwind out of execute() with no one to catch it.
        bool restart = false;
        if (input.contains("restart")) {
            if (!input.at("restart").is_boolean()) {
                return Reject(ctx, started, "restart must be a boolean", command);
            }
            restart = input.at("restart").get<bool>();
        }

        std::int64_t timeout_ms = settings_.timeout_ms;
        if (input.contains("timeout_ms")) {
            if (!input.at("timeout_ms").is_number_integer()) {
                return Reject(ctx, started, "timeout_ms must be an integer", command);
            }
            timeout_ms = input.at("timeout_ms").get<std::int64_t>();
            if (timeout_ms <= 0) {
                return Reject(ctx, started, "timeout_ms must be positive", command);
            }
            timeout_ms = std::min(timeout_ms, settings_.max_timeout_ms);
        }

        // After validation, never before: a rejected invocation must not have
        // wiped the session's cwd and environment on its way out.
        if (restart) store_.Reset(ctx.session_id, ctx.project_root);
        const auto state = store_.Snapshot(ctx.session_id, ctx.project_root);

        const auto outcome =
            RunProcess(WrapCommand(command), state, settings_, timeout_ms, ctx);
        const auto duration_ms = ElapsedMs(started);

        if (outcome.spawn_failed) {
            ctx.logger.warn("bash[{}] spawn failed: {} ({} ms)", ctx.session_id,
                            outcome.spawn_error, duration_ms);
            return Error("failed to start shell: " + outcome.spawn_error);
        }

        if (outcome.state_overflow) {
            ctx.logger.warn("bash[{}] fd-9 state payload exceeded {} bytes; cwd and environment "
                            "left unchanged",
                            ctx.session_id, kMaxStatePayloadBytes);
        } else {
            AdoptShellState(ctx, outcome.state_payload);
        }

        if (outcome.io_error) {
            ctx.logger.warn("bash[{}] io error running '{}': {} ({} ms)", ctx.session_id, command,
                            outcome.io_error_message, duration_ms);
            return Error("failed reading command output: " + outcome.io_error_message);
        }

        // The captured output leads every failure result: it is usually the most
        // useful thing the model can see (the error message, the partial log).
        const auto with_output = [&](const std::string& footer) {
            return outcome.output.empty() ? footer : outcome.output + "\n" + footer;
        };

        if (outcome.cancelled) {
            ctx.logger.info("bash[{}] cancelled '{}' after {} ms", ctx.session_id, command,
                            duration_ms);
            return Error(with_output("[cancelled after " + std::to_string(duration_ms) + " ms]"));
        }
        if (outcome.timed_out) {
            ctx.logger.warn("bash[{}] timed out: '{}' ({} ms)", ctx.session_id, command,
                            duration_ms);
            return Error(with_output("[timed out after " + std::to_string(timeout_ms) +
                                     " ms; process group killed]"));
        }
        if (WIFSIGNALED(outcome.wait_status)) {
            const int sig = WTERMSIG(outcome.wait_status);
            ctx.logger.warn("bash[{}] '{}' terminated by signal {} ({} ms)", ctx.session_id,
                            command, sig, duration_ms);
            return Error(with_output("[terminated by signal " + std::to_string(sig) + "]"));
        }

        const int code = WIFEXITED(outcome.wait_status) ? WEXITSTATUS(outcome.wait_status) : -1;
        if (code == 0) {
            ctx.logger.info("bash[{}] ok: '{}' ({} ms)", ctx.session_id, command, duration_ms);
            return ToolResult{false, outcome.output};
        }
        ctx.logger.warn("bash[{}] exit {}: '{}' ({} ms)", ctx.session_id, code, command,
                        duration_ms);
        return Error(with_output("[exit status: " + std::to_string(code) + "]"));
    }

private:
    void AdoptShellState(ToolContext& ctx, const std::string& payload) {
        if (payload.empty()) return;  // the command exited before the trailer ran
        const ShellState reported = ParseStatePayload(payload);
        if (!reported.ok) {
            ctx.logger.warn("bash[{}] ignoring malformed shell state ({} bytes); "
                            "cwd and environment left unchanged",
                            ctx.session_id, payload.size());
            return;
        }
        store_.SetState(ctx.session_id, reported.cwd, reported.env);
    }

    ToolResult Reject(ToolContext& ctx, std::chrono::steady_clock::time_point started,
                      const std::string& reason, const std::string& command) {
        ctx.logger.info("bash[{}] rejected: {} (command: '{}', {} ms)", ctx.session_id, reason,
                        command, ElapsedMs(started));
        return Error(reason);
    }

    ToolSpec spec_;
    BashSettings settings_;
    BashSessionStateStore& store_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeBashTool(BashSettings settings, BashSessionStateStore& store) {
    return std::make_unique<BashTool>(std::move(settings), store);
}

}  // namespace minimal::tools_bash
