#pragma once

// The generic drain-and-reap runner: poll a launched process's parent-side
// pipe ends into per-pipe sinks until EOF, cancellation, or the deadline;
// then reap, killing the whole group first on any abnormal stop. This is the
// loop every one-shot execution in the tree shares -- bash's synchronous
// path, bash's background watcher, and the python plugin's captured helper
// runs were three hand-rolled copies of it, differing only in per-pipe sink
// policy and cancel source.
//
// Semantics preserved from the originals (see tools-bash's bash_tool.cpp
// history for the reasoning):
//   - Sliced polling so cancellation and the deadline are noticed within one
//     slice, not at the deadline.
//   - EOF on the pipes does NOT mean the child is gone -- it can close its
//     stdio and keep running -- so reaping is deadline-bounded WNOHANG, and
//     an unreaped child gets the group killed before a bounded post-kill
//     wait. "Bounded by SIGKILL" does not hold for a process stuck in
//     uninterruptible disk sleep (D state); the generous ceiling exists so
//     the caller reports an undetermined status instead of hanging forever.
//   - ECHILD is flagged (reap_error), never read as "exit 0": a pid reaped
//     out from under us leaves wait_status meaningless.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/sidecar/process.hpp"

namespace minimal::sidecar {

// One pipe's accumulation policy and result.
struct PipeSink {
    enum class Mode {
        kCapped,   // head+tail CappedBuffer; `emit` (optional) taps live chunks
        kBounded,  // accumulate up to `cap` bytes; past it, latch `overflow`,
                   // clear the payload, and keep draining (discard)
        kDiscard,  // read and throw away (a channel that must drain or the
                   // child blocks writing it, but whose content is unwanted)
    };
    Mode mode = Mode::kDiscard;
    std::int64_t cap = 0;
    std::function<void(std::string_view)> emit;  // kCapped only; may be empty

    // Results (filled by DrainAndReap):
    std::string text;        // kCapped: rendered (with the omission marker);
                             // kBounded: raw payload (empty once overflowed)
    bool overflow = false;   // kBounded only
    std::size_t dropped = 0; // kBounded: bytes beyond the cap
};

struct DrainOptions {
    std::int64_t timeout_ms = 0;
    int slice_ms = 50;             // drain-poll slice: the cancel-latency bound
    int reap_slice_ms = 10;        // WNOHANG poll slice after EOF
    int post_kill_reap_ms = 5000;  // ceiling on the post-kill reap (D-state bound)
    // Fail-fast for a detached grandchild: once the direct child IS reaped, a
    // pipe still open means some grandchild inherited it. 0 (default) keeps
    // today's semantics -- wait for EOF or the deadline. A positive value
    // starts an idle countdown on still-open pipes at that point: any data
    // resets it (a streaming grandchild stays alive, deadline-bounded as
    // ever); quiet for this long closes the pipes, kills the group, and
    // reports detached_grandchild. May only fire AFTER the child is reaped --
    // a live shell that goes quiet (slow build, test pause) is normal and
    // must never be killed by an idle rule.
    std::int64_t idle_after_reap_ms = 0;
};

struct DrainOutcome {
    bool cancelled = false;  // `stop` returned true
    bool timed_out = false;
    bool io_error = false;
    std::string io_error_message;
    // waitpid returned -1 (ECHILD): the pid was reaped out from under us, so
    // wait_status is meaningless and must not be read as "exit 0".
    bool reap_error = false;
    // idle_after_reap_ms fired: the shell was reaped and a grandchild that
    // inherited the output pipe was quiet past the idle window, so the group
    // was killed. wait_status is the SHELL's real status (valid, reaped);
    // the caller appends its own advisory on top of the normal result.
    bool detached_grandchild = false;
    int wait_status = 0;
    std::vector<PipeSink> sinks;  // the sinks, filled
};

// Returns true to stop draining (and kill the group). Polled at the top of
// every slice and inside the reap loop -- a tool's turn cancellation and a
// watcher's session-death poll both come through here.
using StopCheck = std::function<bool()>;

// Takes the Launched by value: it owns the ScopedFds, and after this call
// they have been drained and closed. Always reaps (or flags reap_error) and
// calls ForgetPgid before returning.
DrainOutcome DrainAndReap(Launched proc, std::vector<PipeSink> sinks,
                          const DrainOptions& options, const StopCheck& stop);

}  // namespace minimal::sidecar
