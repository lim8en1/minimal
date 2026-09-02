#include "minimal/client/scrollback_refresh.hpp"

namespace minimal::client {

ScrollbackRefreshAction AdvanceScrollbackRefresh(ScrollbackRefreshState& state,
                                                 const SessionState::Snapshot& snap) {
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
            return ScrollbackRefreshAction::kNone;
        }
        // Failed (or the applied page happened to leave transcript_version
        // where it was): rendered_version stays behind on purpose -- the
        // mismatch check below retries on this very frame.
    }

    if (!state.refresh_pending && state.rendered_version != snap.transcript_version) {
        if (snap.streaming_text.empty() || snap.turn_ended) {
            state.refresh_pending = true;
            state.refresh_seq_seen = snap.refresh_seq;
            state.refresh_from_version = snap.transcript_version;
            return ScrollbackRefreshAction::kSubmit;
        }
        // Mid-turn with live streaming text: the streaming block IS the
        // content until the turn ends; latch rather than fetch around it.
        state.rendered_version = snap.transcript_version;
    }
    return ScrollbackRefreshAction::kNone;
}

}  // namespace minimal::client
