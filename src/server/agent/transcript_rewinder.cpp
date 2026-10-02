#include "minimal/agent/transcript_rewinder.hpp"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include "minimal/agent/injection_queue.hpp"

namespace minimal::agent {

bool IsRewindTarget(const session::StoredMessage& stored) {
    return IsUserTextMessage(stored.message);
}

RewindOutcome TranscriptRewinder::Rewind(std::int64_t target_seq) {
    // Validation reads through the reader, which already filters discarded
    // rows: a rewound-away position comes back as absent and is refused --
    // there is no rewinding into a discarded branch.
    const auto target = reader_.LoadMessage(session_id_, target_seq);
    if (!target || !IsRewindTarget(*target)) {
        return RewindOutcome::Refused("rewind target " + std::to_string(target_seq) +
                                      " is not an active user message");
    }

    // Everything that can fail on a READ happens before the cut: the rebuilt
    // working set is loaded (and trimmed to the kept prefix) first, so a
    // failed load refuses the rewind with the durable transcript untouched
    // instead of reporting "refused" for a cut that already happened. Rows
    // are moved out, never copied -- a long transcript is the case here.
    const auto boundary = loop_.CheckpointBoundary();
    const bool rebuild = boundary.has_value() && target_seq <= *boundary;
    std::vector<session::StoredMessage> kept_rows;
    if (rebuild) {
        kept_rows = reader_.LoadAll(session_id_);
        std::erase_if(kept_rows, [target_seq](const session::StoredMessage& row) {
            return row.seq >= target_seq;
        });
    }
    store_.SetDiscardedAfter(target_seq);

    // TWO memory halves, decided by the checkpoint boundary. Above it, the
    // summary still describes only kept turns and the working set is truncated
    // in place. At or below it, the summary over-covers turns that did not
    // happen -- and dropping it wholesale would also lose the still-active
    // turns BELOW the cut that it summarized, so the working set is rebuilt
    // from the store's active prefix and the checkpoint (the cached view being
    // discarded) is cleared with it. Best-effort, like every checkpoint write:
    // a failed clear degrades to a resume restoring a stale summary, never to
    // a lost transcript.
    if (rebuild) {
        const std::int64_t first_seq = kept_rows.empty() ? 0 : kept_rows.front().seq;
        std::vector<llm::Message> messages;
        messages.reserve(kept_rows.size());
        for (auto& stored : kept_rows) messages.push_back(std::move(stored.message));
        try {
            store_.ClearCompactionCheckpoint();
        } catch (const std::exception& error) {
            logger_.warn("session {}: failed to clear compaction checkpoint: {}", session_id_,
                         error.what());
        }
        loop_.ReseedFromStore(std::move(messages), first_seq);
    } else {
        loop_.TruncateAfter(target_seq);
    }

    RewindOutcome outcome;
    outcome.truncated_to_seq = target_seq;
    // The active transcript now ends just before the target; -1 when the
    // target was seq 0.
    outcome.active_last_seq = target_seq - 1;
    return outcome;
}

RewindOutcome TranscriptRewinder::Redo() {
    // The range to restore is computed BEFORE the unmark: it is the active
    // rows above the current tail, and after the unmark they are active rows
    // too -- indistinguishable from what was never discarded.
    const std::int64_t pre_active_last = reader_.ActiveLastSeq(session_id_);
    const std::int64_t restored = store_.UnmarkDiscarded();
    if (restored == 0) {
        // Either nothing was ever discarded or the fork already reclaimed it.
        // Both mean the same thing to the user: there is no branch to go back
        // to.
        return RewindOutcome::Refused("nothing to redo");
    }

    // The restored rows join the working set WITHOUT being re-appended -- the
    // resume seed's shape, applied to a live loop. LoadAfter reads the
    // just-restored rows; they are active again.
    auto rows = reader_.LoadAfter(session_id_, pre_active_last);
    if (!rows.empty()) {
        const std::int64_t first_seq = rows.front().seq;
        std::vector<llm::Message> messages;
        messages.reserve(rows.size());
        for (auto& stored : rows) messages.push_back(std::move(stored.message));
        loop_.RestorePersisted(std::move(messages), first_seq);
    }

    RewindOutcome outcome;
    outcome.truncated_to_seq = -1;
    outcome.active_last_seq = reader_.ActiveLastSeq(session_id_);
    return outcome;
}

}  // namespace minimal::agent
