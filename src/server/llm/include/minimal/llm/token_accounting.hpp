#pragma once

#include <cstdint>

#include "minimal/llm/message.hpp"

namespace minimal::llm {

// Context occupancy (status-bar used/max, compaction trigger) is the LAST turn's
// input+cache+output, never a running sum across turns: each turn's input_tokens
// already contains the whole history (cache tokens excluded -- providers report
// those separately; see context_occupancy), so summing would double-count
// quadratically and fire compaction far too early.
std::int64_t context_occupancy(const Usage& last_turn);

// True when the backend reported anything at all. Some OpenAI-compatible
// proxies report an all-zero Usage; that is the known gap a caller-side
// fallback estimate (bytes/4) exists to paper over -- this predicate is the
// single place that decides "reported" so the estimate, the honesty rule in
// cost accounting, and the wire flag cannot disagree about it.
bool ReportedUsage(const Usage& usage);

struct CumulativeTotals {
    std::int64_t total_output_tokens = 0;
    std::int64_t total_input_tokens = 0;
};

class TokenAccumulator {
public:
    void RecordTurn(const Usage& turn);
    // Seeds ONLY the last-turn occupancy, never the cumulative totals: resume
    // uses it to tell the compaction trigger how big the loaded working history
    // roughly is, before any provider call has reported real usage. The
    // cumulative totals are genuinely additive quantities, and a synthetic
    // estimate must not pollute them.
    void SeedOccupancy(std::int64_t estimated_input_tokens);
    CumulativeTotals cumulative() const { return cumulative_; }
    std::int64_t last_occupancy() const { return context_occupancy(last_turn_); }
    // The backend's own count of what the last request contained -- ground truth
    // for "how big is the context right now", as opposed to any estimate. 0 when
    // no turn has been recorded yet or the backend reports usage: 0.
    std::int64_t last_input_tokens() const { return last_turn_.input_tokens; }

private:
    CumulativeTotals cumulative_;
    Usage last_turn_;
};

}  // namespace minimal::llm
