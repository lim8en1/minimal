#include "minimal/sidecar/drain.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "minimal/sidecar/capped_buffer.hpp"
#include "minimal/sidecar/policy.hpp"

namespace minimal::sidecar {
namespace {

// kCapped never reaches here: DrainAndReap's read loop dispatches it straight
// to its own CappedBuffer (one per kCapped sink, keyed by position) before
// this switch ever runs.
void FeedSink(PipeSink& sink, std::string_view chunk) {
    switch (sink.mode) {
        case PipeSink::Mode::kCapped:
            break;
        case PipeSink::Mode::kBounded: {
            if (sink.overflow) {
                // Still counted: the caller learns how much was thrown away.
                sink.dropped += chunk.size();
                break;
            }
            if (static_cast<std::int64_t>(sink.text.size()) +
                    static_cast<std::int64_t>(chunk.size()) <=
                sink.cap) {
                sink.text.append(chunk.data(), chunk.size());
            } else {
                // Cleared AND latched: without the flag the payload simply
                // started re-accumulating, so `yes >&9` refilled and cleared
                // 1 MiB over and over for the whole timeout. The payload is
                // discarded wholesale once overflow is set -- adopting a
                // truncated prefix would silently apply partial state.
                sink.overflow = true;
                sink.dropped = sink.text.size() + chunk.size();
                sink.text.clear();
                sink.text.shrink_to_fit();
            }
            break;
        }
        case PipeSink::Mode::kDiscard:
        case PipeSink::Mode::kSource:
            break;
    }
}

// Writes as much of the source's remaining payload as the pipe takes now.
// Returns false once the source is finished -- all written, or the child
// can no longer read it -- and the caller closes it.
#ifndef F_SETNOSIGPIPE
// No per-fd opt-out (Linux): hold SIGPIPE blocked on this thread across the
// write, and consume one it raised, so a child that closed its stdin costs an
// EPIPE rather than the whole process -- whatever the host's disposition.
class ScopedSigpipeBlock {
public:
    ScopedSigpipeBlock() {
        sigset_t pipe_set;
        sigemptyset(&pipe_set);
        sigaddset(&pipe_set, SIGPIPE);
        sigpending(&pending_before_);
        pthread_sigmask(SIG_BLOCK, &pipe_set, &previous_);
    }
    ~ScopedSigpipeBlock() {
        if (!sigismember(&pending_before_, SIGPIPE)) {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE)) {
                sigset_t pipe_set;
                sigemptyset(&pipe_set);
                sigaddset(&pipe_set, SIGPIPE);
                const timespec zero{0, 0};
                while (sigtimedwait(&pipe_set, nullptr, &zero) < 0 && errno == EINTR) {
                }
            }
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t previous_{};
    sigset_t pending_before_{};
};
#endif

bool FeedSource(int fd, const std::string& payload, std::size_t& written) {
#ifndef F_SETNOSIGPIPE
    const ScopedSigpipeBlock sigpipe_guard;
#endif
    while (written < payload.size()) {
        const ssize_t n = write(fd, payload.data() + written, payload.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // pipe full
        return false;  // EPIPE and friends: the child closed its end or exited
    }
    return false;
}

}  // namespace

DrainOutcome DrainAndReap(Launched proc, std::vector<PipeSink> sinks,
                          const DrainOptions& options, const StopCheck& stop) {
    DrainOutcome outcome;
    outcome.sinks = std::move(sinks);
    std::vector<CappedBuffer> capped;
    capped.reserve(outcome.sinks.size());
    for (const auto& sink : outcome.sinks) {
        if (sink.mode == PipeSink::Mode::kCapped) {
            capped.emplace_back(sink.cap, sink.spill);
        } else {
            capped.emplace_back(1);
        }
    }

    const bool budgeted = options.timeout_ms > 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    const auto expired = [&] {
        return budgeted && std::chrono::steady_clock::now() >= deadline;
    };

    // --- Drain: poll every open parent fd, feed its sink --------------------
    std::vector<bool> open(outcome.sinks.size(), true);
    std::vector<std::size_t> written(outcome.sinks.size(), 0);
    for (std::size_t i = 0; i < outcome.sinks.size(); ++i) {
        if (outcome.sinks[i].mode != PipeSink::Mode::kSource) continue;
        if (outcome.sinks[i].payload.empty()) {
            // Closed now, so a child reading stdin sees EOF instead of
            // blocking until the deadline.
            proc.parent_fds[i].reset();
            open[i] = false;
            continue;
        }
        // Non-blocking: a payload larger than the pipe buffer must not park
        // the drain loop in write() while the child waits on its own output.
        const int fd = proc.parent_fds[i].get();
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef F_SETNOSIGPIPE
        fcntl(fd, F_SETNOSIGPIPE, 1);  // EPIPE instead of SIGPIPE where the OS allows
#endif
    }
    bool any_open = !open.empty();
    // idle_after_reap_ms > 0 only: PEEK at the shell each slice -- waitid with
    // WNOWAIT reports "exited (status ready)" WITHOUT reaping, so the pid
    // stays ours (an unreaped zombie cannot be recycled) and the group kill
    // below can never hit a recycled pgid. The real reap happens after.
    bool child_exited = false;
    std::chrono::steady_clock::time_point idle_deadline{};
    while (any_open) {
        if (stop && stop()) {
            outcome.cancelled = true;
            break;
        }
        if (expired()) {
            outcome.timed_out = true;
            break;
        }

        // Sized to the actual sink count, not a fixed cap: this module is
        // shared by every plugin that spawns processes, and a fixed-size
        // array here (there used to be one, pollfd[16]) silently truncated
        // any plan with more pipes than the cap -- the extra pipes were never
        // polled or drained, and their child blocked forever writing to a
        // full, undrained pipe until the overall timeout fired, with nothing
        // explaining why.
        std::vector<pollfd> poll_fds;
        std::vector<int> index_by_slot;
        poll_fds.reserve(outcome.sinks.size());
        index_by_slot.reserve(outcome.sinks.size());
        any_open = false;
        for (std::size_t i = 0; i < outcome.sinks.size(); ++i) {
            if (!open[i]) continue;
            index_by_slot.push_back(static_cast<int>(i));
            const bool source = outcome.sinks[i].mode == PipeSink::Mode::kSource;
            poll_fds.push_back(pollfd{proc.parent_fds[i].get(),
                                      static_cast<short>(source ? POLLOUT : POLLIN), 0});
            any_open = true;
        }
        if (!any_open) break;

        const int rc = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()),
                            static_cast<int>(kDrainSlice.count()));
        if (rc < 0) {
            if (errno == EINTR) continue;
            outcome.io_error = true;
            outcome.io_error_message = std::strerror(errno);
            break;
        }

        bool got_data = false;
        if (rc > 0) {
            char chunk[kPipeReadChunkBytes];
            for (std::size_t slot = 0; slot < poll_fds.size(); ++slot) {
                const std::size_t i = static_cast<std::size_t>(index_by_slot[slot]);
                if (outcome.sinks[i].mode == PipeSink::Mode::kSource) {
                    const short revents = poll_fds[slot].revents;
                    // Hang-up first: the child is gone or closed stdin, and
                    // writing anyway would only earn EPIPE (or SIGPIPE).
                    const bool finished =
                        (revents & (POLLERR | POLLHUP)) != 0 ||
                        ((revents & POLLOUT) != 0 &&
                         !FeedSource(proc.parent_fds[i].get(), outcome.sinks[i].payload,
                                     written[i]));
                    if (finished) {
                        proc.parent_fds[i].reset();
                        open[i] = false;
                    }
                    continue;
                }
                if (!(poll_fds[slot].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const ssize_t n = read(proc.parent_fds[i].get(), chunk, sizeof(chunk));
                if (n > 0) {
                    got_data = true;
                    const std::string_view view(chunk, static_cast<std::size_t>(n));
                    switch (outcome.sinks[i].mode) {
                        case PipeSink::Mode::kCapped:
                            capped[i].Append(view);
                            if (outcome.sinks[i].emit) outcome.sinks[i].emit(view);
                            break;
                        default:
                            FeedSink(outcome.sinks[i], view);
                            break;
                    }
                } else if (n == 0 || errno != EINTR) {
                    open[i] = false;  // EOF (or a persistent error): this pipe is done
                }
            }
        }

        if (options.idle_after_reap_ms <= 0) continue;

        // --- Idle-after-exit accounting (opt-in; see DrainOptions) ----------
        //
        // The shell HAS exited (peeked, still unreaped) and some pipe is still
        // open: a grandchild inherited it. Data resets the countdown (a
        // streaming grandchild stays alive, deadline-bounded as ever); quiet
        // past the window ends the run here instead of at the deadline.
        if (!child_exited) {
            siginfo_t info{};
            const int wrc = waitid(P_PID, proc.pid, &info,
                                   WEXITED | WNOHANG | WNOWAIT);
            if (wrc == 0 && info.si_pid == proc.pid) child_exited = true;
            // EINTR: retry next slice; 0 with si_pid == 0 means "still
            // running" for WNOHANG.
        }
        if (child_exited) {
            bool any_pipe_open = false;       // counts toward the idle-kill below
            bool any_exempt_open = false;     // open, but never a kill reason on its own
            for (std::size_t i = 0; i < open.size(); ++i) {
                if (!open[i] || outcome.sinks[i].mode == PipeSink::Mode::kSource) continue;
                if (outcome.sinks[i].detect_detached_grandchild) {
                    any_pipe_open = true;
                } else {
                    any_exempt_open = true;
                }
            }
            if (any_pipe_open) {
                if (got_data || idle_deadline.time_since_epoch().count() == 0) {
                    idle_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(options.idle_after_reap_ms);
                }
                if (std::chrono::steady_clock::now() >= idle_deadline) {
                    outcome.detached_grandchild = true;
                    break;
                }
            } else {
                idle_deadline = {};
                if (any_exempt_open) {
                    // Every WATCHED pipe is closed; only an exempt one (bash's
                    // fd-9 state channel) remains, held by a grandchild that
                    // doesn't even know the fd exists and so will never close
                    // it. Waiting further only costs time up to the overall
                    // deadline for a close that is never coming -- stop here,
                    // exactly as if it had hit EOF like the others.
                    break;
                }
            }
        }
    }

    // --- Reap ------------------------------------------------------------------
    //
    // For a natural exit this is deadline-bounded WNOHANG slices -- EOF on the
    // pipes does not mean the process is gone (it can close its stdio and keep
    // running) -- with the same stop/deadline checks the drain loop had. For a
    // cancelled or timed-out task the kill comes first and the reap is bounded
    // by SIGKILL's ceiling (the D-state corner: a pending SIGKILL does not
    // bound a blocking waitpid, so poll WNOHANG instead).
    int status = 0;
    bool reaped = false;
    if (!outcome.cancelled && !outcome.timed_out && !outcome.io_error &&
        !outcome.detached_grandchild) {
        while (true) {
            const pid_t rc = waitpid(proc.pid, &status, WNOHANG);
            if (rc == proc.pid) {
                reaped = true;
                break;
            }
            if (rc < 0) {
                if (errno == EINTR) continue;
                reaped = false;
                outcome.reap_error = true;  // ECHILD: status meaningless
                break;
            }
            if (stop && stop()) {
                outcome.cancelled = true;
                break;
            }
            if (expired()) {
                outcome.timed_out = true;
                break;
            }
            poll(nullptr, 0, static_cast<int>(kReapPollInterval.count()));
        }
    }

    // The group kill ordering is load-bearing for the detached-grandchild
    // path: the shell was only PEEKED (never reaped early), so its pid --
    // and pgid, the shell setsid()s -- cannot have been recycled. killpg
    // reaps-first would race pid reuse and could SIGKILL an unrelated,
    // recycled process group.
    // Not after a reap_error (ECHILD): the child is already gone, so the pid may
    // have been recycled and killpg could hit an unrelated process group.
    if ((!reaped && !outcome.reap_error) || outcome.detached_grandchild) {
        KillProcessGroup(proc.pid);
    }
    if (!reaped && !outcome.reap_error) {
        const auto kill_deadline = std::chrono::steady_clock::now() + kPostKillReapCeiling;
        while (std::chrono::steady_clock::now() < kill_deadline) {
            const pid_t rc = waitpid(proc.pid, &status, WNOHANG);
            if (rc == proc.pid) {
                reaped = true;
                break;
            }
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) {
                outcome.reap_error = true;  // nothing left to reap; status meaningless
                break;
            }
            poll(nullptr, 0, static_cast<int>(kReapPollInterval.count()));
        }
        if (!reaped && !outcome.reap_error) {
            // The kill did not land within the ceiling (D state): report the
            // status as undetermined instead of hanging, and instead of
            // reading the zeroed status as a clean exit.
            outcome.reap_error = true;
        }
    }
    ForgetPgid(proc.pid);

    outcome.wait_status = status;
    for (std::size_t i = 0; i < outcome.sinks.size(); ++i) {
        if (outcome.sinks[i].mode == PipeSink::Mode::kCapped) {
            outcome.sinks[i].text = capped[i].Render();
            outcome.sinks[i].total = capped[i].total();
        }
    }
    return outcome;
}

}  // namespace minimal::sidecar
