#include "minimal/llm/token_accounting.hpp"

namespace minimal::llm {

std::int64_t context_occupancy(const Usage& last_turn) {
    return static_cast<std::int64_t>(last_turn.input_tokens) + last_turn.output_tokens;
}

void TokenAccumulator::RecordTurn(const Usage& turn) {
    cumulative_.total_output_tokens += turn.output_tokens;
    cumulative_.total_input_tokens += turn.input_tokens;
    last_turn_ = turn;
}

}  // namespace minimal::llm
