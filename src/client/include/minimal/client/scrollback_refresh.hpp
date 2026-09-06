#pragma once

// The frame pass's newest-slice refresh decision (app.cpp's maintain()).
// Extracted because this is where the "user prompt seen only on the first
// message" bug lived: three state machines (version mismatch, in-flight
// request, streaming/turn boundary) interlocked so the turn-end version bump
// was consumed by a latch that no fetch ever resolved. Pure value logic over
// the Snapshot -- no FTXUI, no AppState -- so each interlock is a test case.
//
// This is NOT in chat_model.hpp because it needs SessionState's definition,
// and state.hpp includes chat_model.hpp (a cycle otherwise).

#include <chrono>
#include <cstdint>
#include <optional>

#include "minimal/client/state.hpp"

namespace minimal::client {

// Per-tab bookkeeping for AdvanceScrollbackRefresh (TabUi's refresh fields).
// Plain value state; app.cpp holds one per open tab.
struct ScrollbackRefreshState {
    // The transcript version the rendered rows correspond to. nullopt =
    // nothing fetched yet (a fresh tab fetches on its first frame).
    std::optional<uint64_t> rendered_version;
    bool refresh_pending = false;       // an async newest-fetch is outstanding
    uint64_t refresh_seq_seen = 0;      // pre-submit Snapshot::refresh_seq
    uint64_t refresh_from_version = 0;  // pre-submit Snapshot::transcript_version

    // Failure backoff (see AdvanceScrollbackRefresh): consecutive RESOLVED
    // FAILURES, and the earliest steady-clock time the NEXT submit may fire.
    // The first failure still retries on the same frame (the retry contract
    // below is unchanged); from the second failure on, submits are held
    // 250 ms, doubling per further failure, capped at 5 s. Reset on success.
    // Without this, a dead server ate hundreds of connect attempts per
    // second -- every frame retried the moment the failure resolved.
    int consecutive_failures = 0;
    std::chrono::steady_clock::time_point next_retry_earliest{};
};

enum class ScrollbackRefreshAction {
    kNone,    // nothing to do this frame
    kSubmit,  // call AppState::RequestScrollback(session_id, newest=true)
};

// One maintain() pass over one tab's refresh bookkeeping. Resolves an
// outstanding request (Snapshot::refresh_seq moved) before considering a new
// one, then acts on a version mismatch:
//   - a resolved SUCCESSFUL request marks the rendered version current, even
//     when the page applied no rows (an empty page moves no version -- without
//     this, a fresh tab retried GetMessages every frame until the first
//     message persisted);
//   - a resolved FAILED request leaves the mismatch so the SAME frame
//     resubmits (the retry contract);
//   - a mismatch fetches whenever the turn boundary allows it: streaming text
//     empty (idle) OR the turn has ended. The accumulated streaming text is
//     cleared only BY the applying refresh, so "text empty" cannot tell
//     "turn over" from "mid-turn" -- Snapshot::turn_ended can, and that is the
//     fix: the turn-end version bump now reaches a fetch instead of being
//     latched away;
//   - a mismatch while a turn is genuinely in flight latches (the streaming
//     block IS the live content; fetching around it would swap it out
//     mid-stream);
//   - from the SECOND consecutive failure on, resubmits are backed off
//     exponentially (250 ms doubling, 5 s cap; reset on success) so a dead
//     server is not hammered every frame -- the first failure keeps the
//     same-frame retry contract.
//
// `now` is injectable for tests; default is the real clock.
ScrollbackRefreshAction AdvanceScrollbackRefresh(
    ScrollbackRefreshState& state, const SessionState::Snapshot& snap,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

}  // namespace minimal::client
