#include "cost_observer.hpp"

#include <chrono>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::cost_tracker {

void CostSessionObserver::OnTurnCompleted(const agent::SessionInfo& info,
                                          const agent::TurnSummary& summary) {
    TurnRecord record;
    record.model = summary.model;
    record.at_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    record.usage.input_tokens = summary.turn_usage.input_tokens;
    record.usage.output_tokens = summary.turn_usage.output_tokens;
    record.usage.cache_read_tokens = summary.turn_usage.cache_read_input_tokens;
    record.usage.cache_write_tokens = summary.turn_usage.cache_creation_input_tokens;
    record.usage.usage_reported =
        // A turn with ANY unreported round (the zero-usage proxy gap's
        // bytes/4 fallback) prices as unreported even though its other
        // rounds reported: the sum is a lower bound, and billing a partial
        // count as whole would understate silently. The estimate exists to
        // arm the compaction trigger, nothing else -- the honesty rule
        // ("unknown", never a fabricated $0 or a fabricated $X) applies to
        // the whole turn, not per round.
        summary.turn_usage_fully_reported &&
        (summary.turn_usage.input_tokens > 0 ||
         summary.turn_usage.output_tokens > 0 ||
         summary.turn_usage.cache_read_input_tokens > 0 ||
         summary.turn_usage.cache_creation_input_tokens > 0);
    record.first_seq = summary.first_seq;
    record.last_seq = summary.last_seq;
    try {
        store_.AppendTurn(info.session_id, info.parent_session_id, record);
    } catch (const std::exception& error) {
        // Observers are plugin code; one failed append must never stop the
        // others or wedge the dispatch thread. The cost history loses a turn;
        // the session does not notice.
        common::DefaultLogger().warn("cost-tracker: recording turn for session {} failed: {}",
                                     info.session_id, error.what());
    }
    // The turn's provisional rounds are now superseded by the authoritative
    // record -- dropped whether or not the append succeeded, so the totals
    // RPC can never count a round twice (live view + turn record).
    in_flight_.Clear(info.session_id);
    Changed(info);
}

void CostSessionObserver::OnBilledCall(const agent::SessionInfo& info,
                                       const agent::BilledCall& call) {
    TurnRecord record;
    record.model = call.model;
    record.at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    record.usage.input_tokens = call.usage.input_tokens;
    record.usage.output_tokens = call.usage.output_tokens;
    record.usage.cache_read_tokens = call.usage.cache_read_input_tokens;
    record.usage.cache_write_tokens = call.usage.cache_creation_input_tokens;
    // The honesty rule: an estimated count is "unknown", never a price.
    record.usage.usage_reported =
        !call.usage_estimated &&
        (call.usage.input_tokens > 0 || call.usage.output_tokens > 0 ||
         call.usage.cache_read_input_tokens > 0 || call.usage.cache_creation_input_tokens > 0);
    // first_seq/last_seq stay -1: no transcript range, so a rewind keeps it.
    try {
        store_.AppendTurn(info.session_id, info.parent_session_id, record);
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("cost-tracker: recording a {} call for session {} failed: {}",
                                     call.purpose, info.session_id, error.what());
    }
    Changed(info);
}

void CostSessionObserver::OnProviderRound(const agent::SessionInfo& info,
                                          const agent::RoundUsage& round) {
    // No throw here to catch: the in-flight tracker's RecordRound allocates a
    // few small nodes, and one provisional round lost to a bad_alloc is
    // cosmetic. Everything fallible stays in OnTurnCompleted's path.
    in_flight_.RecordRound(info.session_id, round);
    Changed(info);
}

void CostSessionObserver::OnSessionRewound(const agent::SessionInfo& info,
                                           std::int64_t truncated_to_seq) {
    // Redo (truncated_to_seq < 0): the discarded tail came back, but which
    // stored records it maps to is unknowable here -- nothing to restore.
    if (truncated_to_seq < 0) return;
    try {
        SessionHistory history = store_.Load(info.session_id);
        if (history.turns.empty()) return;
        // RunRewind marks rows with seq >= truncated_to_seq discarded (the cut
        // lands ON the target user message; the transcript ends just before
        // it), so a turn is gone when its LAST row is at or past the cut.
        // Sentinel -1 records predate seq bookkeeping: unclassifiable, kept.
        std::vector<TurnRecord> kept;
        kept.reserve(history.turns.size());
        for (auto& turn : history.turns) {
            if (turn.last_seq < 0 || turn.last_seq < truncated_to_seq) {
                kept.push_back(std::move(turn));
            }
        }
        if (kept.size() == history.turns.size()) return;  // nothing past the cut
        history.turns = std::move(kept);
        store_.Save(info.session_id, history);
        Changed(info);
    } catch (const std::exception& error) {
        // Same observer contract as OnTurnCompleted: never wedge the dispatch
        // thread. /cost may over-report until the next successful rewind.
        common::DefaultLogger().warn("cost-tracker: applying rewind for session {} failed: {}",
                                     info.session_id, error.what());
    }
}

void CostSessionObserver::OnSessionEnded(const agent::SessionInfo& info) {
    // A session never ends mid-turn, so this normally finds nothing -- but a
    // provisional total outliving its session would be a slow leak keyed by
    // dead ids.
    in_flight_.Clear(info.session_id);
}

void CostSessionObserver::Changed(const agent::SessionInfo& info) {
    if (index_ != nullptr) index_->Note(info.session_id, info.parent_session_id);
    if (hub_ != nullptr) hub_->MarkDirty(info.session_id);
}

}  // namespace minimal::cost_tracker
