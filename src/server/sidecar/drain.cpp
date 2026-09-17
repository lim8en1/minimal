#include "minimal/sidecar/drain.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#include "minimal/sidecar/capped_buffer.hpp"

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
            break;
    }
}

}  // namespace

DrainOutcome DrainAndReap(Launched proc, std::vector<PipeSink> sinks,
                          const DrainOptions& options, const StopCheck& stop) {
    DrainOutcome outcome;
    outcome.sinks = std::move(sinks);
    std::vector<CappedBuffer> capped;
    capped.reserve(outcome.sinks.size());
    for (const auto& sink : outcome.sinks) {
        capped.emplace_back(sink.mode == PipeSink::Mode::kCapped ? sink.cap : 1);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    const auto expired = [&] {
        return std::chrono::steady_clock::now() >= deadline;
    };

    // --- Drain: poll every open parent fd, feed its sink --------------------
    std::vector<bool> open(outcome.sinks.size(), true);
    bool any_open = !open.empty();
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
            poll_fds.push_back(pollfd{proc.parent_fds[i].get(), POLLIN, 0});
            any_open = true;
        }
        if (!any_open) break;

        const int rc = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), options.slice_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            outcome.io_error = true;
            outcome.io_error_message = std::strerror(errno);
            break;
        }
        if (rc == 0) continue;

        char chunk[8192];
        for (std::size_t slot = 0; slot < poll_fds.size(); ++slot) {
            if (!(poll_fds[slot].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const std::size_t i = static_cast<std::size_t>(index_by_slot[slot]);
            const ssize_t n = read(proc.parent_fds[i].get(), chunk, sizeof(chunk));
            if (n > 0) {
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
    if (!outcome.cancelled && !outcome.timed_out && !outcome.io_error) {
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
            poll(nullptr, 0, options.reap_slice_ms);
        }
    }

    if (!reaped) KillProcessGroup(proc.pid);
    if (!reaped && !outcome.reap_error) {
        const auto kill_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(options.post_kill_reap_ms);
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
            poll(nullptr, 0, options.reap_slice_ms);
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
        }
    }
    return outcome;
}

}  // namespace minimal::sidecar
