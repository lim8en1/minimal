#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::agent {

// The outcome of a successful compaction. original_token_count is the size of
// what was replaced: the backend's own count from the last API call when it
// reported one (ground truth), else a bytes/4 estimate -- there is no tokenizer
// in this codebase, and an estimate that says so on its sleeve beats a precise
// number computed from nothing.
struct CompactionResult {
    llm::Message summary_message;
    std::int64_t original_token_count = 0;
};

// The production compactor: summarizes `messages_to_compact` into one synthetic
// message by calling the provider with `compression_prompt` as the system prompt.
// The messages go through verbatim -- tool blocks included -- because both wire
// formats accept them in history without a tools array, and flattening would lose
// fidelity for more code than it saves.
//
// Returns nullopt when no compaction should happen: empty prompt, provider error,
// cancellation, or a response with no usable text. Failure is a value, not an
// exception -- the caller leaves history untouched, so a broken summarizer degrades
// to "no compaction", never to a lost transcript.
std::optional<CompactionResult> compact_messages(
    const std::vector<llm::Message>& messages_to_compact, llm::LlmProvider& provider,
    std::string_view model_name, std::string_view compression_prompt,
    std::int64_t last_reported_input_tokens, std::function<bool()> is_cancelled);

}  // namespace minimal::agent
