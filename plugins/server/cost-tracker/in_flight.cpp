#include "in_flight.hpp"

namespace minimal::cost_tracker {

void InFlightUsage::RecordRound(const std::string& session_id,
                                const agent::RoundUsage& round) {
    const std::lock_guard<std::mutex> lock(mtx_);
    auto& model_usage = sessions_[session_id][round.model];
    if (round.usage_estimated) {
        // The bytes/4 fallback's tokens are fabricated: record that the round
        // HAPPENED (the flag), never its counts -- the same rule
        // cost_observer.cpp applies to a turn whose only round was estimated.
        model_usage.fully_reported = false;
        return;
    }
    model_usage.usage.input_tokens += round.usage.input_tokens;
    model_usage.usage.output_tokens += round.usage.output_tokens;
    model_usage.usage.cache_read_input_tokens += round.usage.cache_read_input_tokens;
    model_usage.usage.cache_creation_input_tokens +=
        round.usage.cache_creation_input_tokens;
}

void InFlightUsage::Clear(const std::string& session_id) {
    const std::lock_guard<std::mutex> lock(mtx_);
    sessions_.erase(session_id);
}

std::vector<TurnRecord> InFlightUsage::Records(const std::string& session_id) const {
    const std::lock_guard<std::mutex> lock(mtx_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return {};
    std::vector<TurnRecord> records;
    records.reserve(it->second.size());
    for (const auto& [model, model_usage] : it->second) {
        TurnRecord record;
        record.model = model;
        record.usage.input_tokens = model_usage.usage.input_tokens;
        record.usage.output_tokens = model_usage.usage.output_tokens;
        record.usage.cache_read_tokens = model_usage.usage.cache_read_input_tokens;
        record.usage.cache_write_tokens = model_usage.usage.cache_creation_input_tokens;
        record.usage.usage_reported =
            model_usage.fully_reported &&
            (model_usage.usage.input_tokens > 0 ||
             model_usage.usage.output_tokens > 0 ||
             model_usage.usage.cache_read_input_tokens > 0 ||
             model_usage.usage.cache_creation_input_tokens > 0);
        records.push_back(std::move(record));
    }
    return records;
}

}  // namespace minimal::cost_tracker
