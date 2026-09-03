#include "minimal/llm/token_accounting.hpp"

namespace minimal::llm {

std::int64_t context_occupancy(const Usage& last_turn) {
    // Cache tokens ARE context: providers report input_tokens as the UNCACHED
    // portion (Anthropic's semantics, which provider-openai's ParseUsage now
    // normalizes to), so occupancy must add back what was served from / written
    // to the prompt cache or status-bar "used" and the compaction trigger
    // under-count cached sessions.
    return static_cast<std::int64_t>(last_turn.input_tokens) + last_turn.output_tokens +
           last_turn.cache_read_input_tokens + last_turn.cache_creation_input_tokens;
}

void TokenAccumulator::RecordTurn(const Usage& turn) {
    cumulative_.total_output_tokens += turn.output_tokens;
    cumulative_.total_input_tokens += turn.input_tokens;
    last_turn_ = turn;
}

void TokenAccumulator::SeedOccupancy(std::int64_t estimated_input_tokens) {
    // No cumulative bookkeeping, deliberately -- see the header. The value is
    // clamped into the Usage field's width; no real context window approaches
    // INT_MAX tokens, so the cast loses nothing in practice.
    last_turn_ = Usage{};
    last_turn_.input_tokens = static_cast<int>(estimated_input_tokens);
}

}  // namespace minimal::llm
