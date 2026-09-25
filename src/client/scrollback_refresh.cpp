#include "minimal/client/scrollback_refresh.hpp"

#include <algorithm>

namespace minimal::client {

namespace {
// Backoff schedule for consecutive failed refreshes (see
// ScrollbackRefreshState): 250 ms base, doubling per further failure, 5 s
// cap. Applied only from the SECOND consecutive failure on -- a single
// failure still retries on the same frame, which is what keeps the "the
// next frame retries" contract below intact for a merely-slow server.
constexpr auto kRetryBackoffBase = std::chrono::milliseconds(250);
constexpr auto kRetryBackoffMax = std::chrono::milliseconds(5000);
}  // namespace

ScrollbackRefreshAction AdvanceScrollbackRefresh(ScrollbackRefreshState& state,
                                                 const SessionState::Snapshot& snap,
                                                 std::chrono::steady_clock::time_point now) {
    // Resolve any outstanding refresh BEFORE deciding whether to submit a
    // new one: refresh_seq (bumped by SessionState on every RESOLVED attempt
    // -- success or failure) is a different counter from fetch_generation
    // specifically so a failed fetch still tells this frame "stop waiting",
    // without looking identical to "still in flight".
    if (state.refresh_pending && snap.refresh_seq != state.refresh_seq_seen) {
        state.refresh_pending = false;
        if (snap.refresh_succeeded) {
            // A page applied (or an empty page resolved successfully): the
            // window now covers everything persisted up to the generation
            // this fetch was SUBMITTED at. Not the snapshot's generation --
            // a trigger that landed while the fetch was in flight (turn end
            // during a mid-turn fetch) may name rows this fetch never saw;
            // marking it handled here was the "tool rows vanish until
            // restart" bug. Such a trigger leaves a mismatch below and gets
            // its own fetch; with none, this settles (no empty-page loop).
            state.fetched_generation = state.refresh_from_generation;
            state.consecutive_failures = 0;   // backoff resets on success
            // Falls through to the mismatch check on THIS frame: an idle tab
            // may render no further frame to notice the pending trigger.
        } else {
            // Failed: fetched_generation stays behind on purpose -- the
            // mismatch check below retries on this very frame.
            ++state.consecutive_failures;
            // From the second consecutive failure on, hold the NEXT submit:
            // a dead server otherwise gets a connect attempt per frame
            // (20 Hz), because the failure resolves the latch without moving
            // fetch_generation, and the unchanged mismatch resubmits
            // immediately. The first failure stays un-delayed (retry
            // contract).
            if (state.consecutive_failures >= 2) {
                const int shift = std::min(state.consecutive_failures - 2, 10);
                state.next_retry_earliest =
                    now + std::min(kRetryBackoffBase * (1 << shift), kRetryBackoffMax);
            }
        }
    }

    if (!state.refresh_pending && state.fetched_generation != snap.fetch_generation) {
        // No turn-boundary latch: a mismatch fetches whenever the backoff
        // window allows it, MID-TURN included. A tool round finishing bumps
        // the version while the turn keeps streaming, and the fetch that
        // follows is safe mid-turn by construction -- ApplyScrollbackPage's
        // content-matched retirement swaps in exactly the persisted rows
        // that cover the finished rounds' live segments, while anything
        // still open or running stays live below them. (The old latch --
        // "the streaming block IS the content until the turn ends" -- was
        // what made live tool rows structurally different from their
        // persisted twins: expanded and non-collapsible while streaming,
        // collapsed only at turn end or on a mid-turn submit's eager fetch.)
        if (state.consecutive_failures < 2 || now >= state.next_retry_earliest) {
            state.refresh_pending = true;
            state.refresh_seq_seen = snap.refresh_seq;
            state.refresh_from_generation = snap.fetch_generation;
            return ScrollbackRefreshAction::kSubmit;
        }
    }
    return ScrollbackRefreshAction::kNone;
}

}  // namespace minimal::client
