#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::agent {

// The outcome of a successful compaction.
//
// context_tokens_before is the size of the WHOLE context going into the request
// that triggered compaction -- system prompt, tool definitions, retained tail and
// all -- not the size of the range that was replaced. That distinction was worth
// a rename: the field used to be called original_token_count and its two branches
// measured different things (the backend's whole-request count on one path, a
// bytes/4 estimate of just the compacted range on the other), so the number the
// log printed as "what compaction replaced" silently overstated by roughly the
// size of everything it kept.
//
// Ground truth when the backend reported a count, else a bytes/4 estimate over
// the same whole context -- there is no tokenizer in this codebase, and an
// estimate that says so on its sleeve beats a precise number computed from
// nothing.
struct CompactionResult {
    llm::Message summary_message;
    std::int64_t context_tokens_before = 0;
};

// Everything the summarization call needs that varies per call. A struct rather
// than eight positional parameters, and per-call rather than bound at
// construction because /agent can switch presets mid-session -- binding would
// compact using the previous preset's prompt and sampling settings.
struct CompactionRequest {
    std::string_view model_name;
    std::string_view compression_prompt;

    // The preset's own sampling knobs and output cap. Both are load-bearing
    // rather than tidiness: without `parameters` the summarization call runs
    // with different generation settings than every other call in the session,
    // and the OpenAI serializer ERASES max_tokens when the request does not set
    // it -- so a cap the user put in `parameters` was being stripped on this
    // path specifically, which is how a summary gets truncated.
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;

    // The backend's count for the last real request, or 0 if it reported none.
    std::int64_t last_reported_input_tokens = 0;
    // Everything currently in context, used only for the fallback estimate when
    // the backend reported nothing. Measured over the same span as the ground
    // truth so the two branches are comparable.
    const std::vector<llm::Message>* full_history = nullptr;

    // Where the compactor's own diagnostics go. Null means DefaultLogger --
    // which is what every call used to get, so its lines carried no session id
    // while the agent loop's adjacent compaction log did. Callers that have a
    // session pass both this and `session_label`.
    const common::Logger* logger = nullptr;
    // Prefixed onto the compactor's log lines ("session <id>: ..."), matching
    // the agent loop's format so the two interleaved streams read as one.
    std::string_view session_label;

    std::function<bool()> is_cancelled;
};

// The production compactor: summarizes `messages_to_compact` into one synthetic
// message by calling the provider with `compression_prompt` as the system prompt.
// The messages go through verbatim -- tool blocks included -- because both wire
// formats accept them in history without a tools array, and flattening would lose
// fidelity for more code than it saves.
//
// Returns nullopt when no compaction should happen: empty prompt, provider error,
// cancellation, a response with no usable text, or a summary the model ran out of
// output budget mid-way through. Failure is a value, not an exception -- the
// caller leaves history untouched, so a broken summarizer degrades to "no
// compaction", never to a lost transcript.
std::optional<CompactionResult> compact_messages(
    std::span<const llm::Message> messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& request);

// Rough size of a message range in tokens: serialized bytes / 4. The fallback
// for context_tokens_before when the backend reported no usage -- an estimate
// that says so on its sleeve beats a precise number computed from nothing.
// Exposed beyond the compactor because resume has the same problem: it seeds
// the compaction trigger with a loaded transcript and no backend count either.
std::int64_t estimate_tokens(std::span<const llm::Message> messages);

// Serialized size of a JSON value, without serializing it. Structural
// characters are counted the way dump() would emit them; the point is a stable
// approximation, not byte equality with a string nobody keeps. Exposed because
// the occupancy estimate has the same shape as the compactor's fallback --
// sized, not serialized -- over the tools array's input schemas.
std::int64_t JsonSizeEstimate(const nlohmann::json& value);

}  // namespace minimal::agent
