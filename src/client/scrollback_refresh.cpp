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
    // -- success or failure) is a different counter from transcript_version
    // specifically so a failed fetch still tells this frame "stop waiting",
    // without looking identical to "still in flight".
    if (state.refresh_pending && snap.refresh_seq != state.refresh_seq_seen) {
        state.refresh_pending = false;
        if (snap.refresh_succeeded) {
            // A page applied (or an empty page resolved successfully): this
            // snapshot was taken after the worker's BumpUiVersion woke the
            // pump, so it already reflects the attempt. Mark current even
            // when the version did not move -- that is exactly the empty-page
            // case, and NOT marking it is the every-frame refetch loop.
            state.rendered_version = snap.transcript_version;
            state.consecutive_failures = 0;   // backoff resets on success
            return ScrollbackRefreshAction::kNone;
        }
        // Failed (or the applied page happened to leave transcript_version
        // where it was): rendered_version stays behind on purpose -- the
        // mismatch check below retries on this very frame.
        ++state.consecutive_failures;
        // From the second consecutive failure on, hold the NEXT submit: a
        // dead server otherwise gets a connect attempt per frame (20 Hz),
        // because the failure resolves the latch without moving
        // transcript_version, and the unchanged mismatch resubmits
        // immediately. The first failure stays un-delayed (retry contract).
        if (state.consecutive_failures >= 2) {
            const int shift = std::min(state.consecutive_failures - 2, 10);
            state.next_retry_earliest =
                now + std::min(kRetryBackoffBase * (1 << shift), kRetryBackoffMax);
        }
    }

    if (!state.refresh_pending && state.rendered_version != snap.transcript_version) {
        if (snap.streaming_text.empty() || snap.turn_ended) {
            // Backoff gate: a failure streak holds resubmits until the
            // window passes (or a NEW failure resolves, extending it). Not
            // latching refresh_pending here means the mismatch simply
            // survives; the next frame after the window submits.
            if (state.consecutive_failures < 2 || now >= state.next_retry_earliest) {
                state.refresh_pending = true;
                state.refresh_seq_seen = snap.refresh_seq;
                state.refresh_from_version = snap.transcript_version;
                return ScrollbackRefreshAction::kSubmit;
            }
        } else {
            // Mid-turn with live streaming text: the streaming block IS the
            // content until the turn ends; latch rather than fetch around it.
            state.rendered_version = snap.transcript_version;
        }
    }
    return ScrollbackRefreshAction::kNone;
}

}  // namespace minimal::client
