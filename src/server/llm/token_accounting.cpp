#include "minimal/llm/token_accounting.hpp"

#include <algorithm>
#include <limits>

namespace minimal::llm {

std::int64_t context_occupancy(const Usage& last_turn) {
    // Cache tokens ARE context: providers report input_tokens as the UNCACHED
    // portion (Anthropic's semantics, which provider-openai's ParseUsage now
    // normalizes to), so occupancy must add back what was served from / written
    // to the prompt cache or status-bar "used" and the compaction trigger
    // under-count cached sessions. Fields are clamped at zero: a provider that
    // reports a negative value (a sentinel, a misparse, a hostile gateway)
    // must not LOWER occupancy below reality -- the trigger compares
    // occupancy < threshold, so negative usage would silently disable
    // compaction for the whole session.
    return std::max<std::int64_t>(last_turn.input_tokens, 0) +
           std::max<std::int64_t>(last_turn.output_tokens, 0) +
           std::max<std::int64_t>(last_turn.cache_read_input_tokens, 0) +
           std::max<std::int64_t>(last_turn.cache_creation_input_tokens, 0);
}

void TokenAccumulator::RecordTurn(const Usage& turn) {
    cumulative_.total_output_tokens += turn.output_tokens;
    cumulative_.total_input_tokens += turn.input_tokens;
    last_turn_ = turn;
}

void TokenAccumulator::SeedOccupancy(std::int64_t estimated_input_tokens) {
    // No cumulative bookkeeping, deliberately -- see the header. Saturate,
    // don't truncate: the comment long claimed a clamp while the bare
    // static_cast wrapped (a >INT_MAX estimate came back as a small or
    // negative number, mis-seeding compaction). No real context window
    // approaches INT_MAX tokens, so saturation loses nothing.
    last_turn_ = Usage{};
    last_turn_.input_tokens =
        static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(estimated_input_tokens, 0),
                                                std::numeric_limits<int>::max()));
}

}  // namespace minimal::llm
