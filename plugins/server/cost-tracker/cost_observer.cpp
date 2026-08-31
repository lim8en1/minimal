#include "cost_observer.hpp"

#include <chrono>

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

}  // namespace minimal::cost_tracker
