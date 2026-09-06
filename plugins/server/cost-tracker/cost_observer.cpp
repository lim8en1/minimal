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
    record.usage.input_tokens = summary.usage.input_tokens;
    record.usage.output_tokens = summary.usage.output_tokens;
    record.usage.cache_read_tokens = summary.usage.cache_read_input_tokens;
    record.usage.cache_write_tokens = summary.usage.cache_creation_input_tokens;
    record.usage.usage_reported = (summary.usage.input_tokens > 0 ||
                                   summary.usage.output_tokens > 0 ||
                                   summary.usage.cache_read_input_tokens > 0 ||
                                   summary.usage.cache_creation_input_tokens > 0);
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
    } catch (const std::exception& error) {
        // Same observer contract as OnTurnCompleted: never wedge the dispatch
        // thread. /cost may over-report until the next successful rewind.
        common::DefaultLogger().warn("cost-tracker: applying rewind for session {} failed: {}",
                                     info.session_id, error.what());
    }
}

}  // namespace minimal::cost_tracker
