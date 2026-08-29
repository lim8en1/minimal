#pragma once

#include <cstdint>

#include "minimal/llm/message.hpp"

namespace minimal::llm {

// Context occupancy (status-bar used/max, compaction trigger) is the LAST turn's
// input+output, never a running sum across turns: each turn's input_tokens
// already contains the whole history, so summing would double-count
// quadratically and fire compaction far too early.
std::int64_t context_occupancy(const Usage& last_turn);

struct CumulativeTotals {
    std::int64_t total_output_tokens = 0;
    std::int64_t total_input_tokens = 0;
};

class TokenAccumulator {
public:
    void RecordTurn(const Usage& turn);
    CumulativeTotals cumulative() const { return cumulative_; }
    std::int64_t last_occupancy() const { return context_occupancy(last_turn_); }

private:
    CumulativeTotals cumulative_;
    Usage last_turn_;
};

}  // namespace minimal::llm
