#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::agent {

// The fixed line every summary head opens with, free-text and structured
// alike -- declared here rather than in either .cpp so the two compacters
// cannot drift (the resumed working set must not depend on which strategy
// happened to write the head).
inline constexpr std::string_view kSummaryFraming =
    "This conversation was compacted. The summary below covers the whole session up to the "
    "compaction; the most recent messages follow it verbatim.\n\n";

// Whitespace-only check shared by both compacters' refusal chains.
inline bool IsBlank(std::string_view text) {
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

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
    // The instruction appended after the conversation, verbatim -- the text
    // lives in the config dir's prompts/, not here. The free-text compactor
    // sends compression_prompt; the structured one sends structured_prompt,
    // unless `custom_prompt` is set or structured_prompt is blank, in which
    // case it degrades to free text.
    std::string_view compression_prompt;
    std::string_view structured_prompt;
    // True when compression_prompt is not the bundled default -- a preset's
    // `compression_prompt:` frontmatter or a /compact <name> override. A
    // custom prompt describes its own output shape, so it runs unstructured.
    bool custom_prompt = false;

    // The session's own request prefix -- system prompt and tool definitions,
    // exactly as the main loop sends them. The summarizer request is that
    // request plus one appended instruction message, so a provider with
    // prefix caching reads the whole history from cache. Both may be empty
    // (direct-API callers, tests): the request then carries no system/tools.
    std::optional<std::string> system;
    std::vector<llm::ToolDefinition> tools;

    // The preset's own sampling knobs and output cap. Both are load-bearing
    // rather than tidiness: without `parameters` the summarization call runs
    // with different generation settings than every other call in the session,
    // and the OpenAI serializer ERASES max_tokens when the request does not set
    // it -- so a cap the user put in `parameters` was being stripped on this
    // path specifically, which is how a summary gets truncated.
    nlohmann::json parameters = nlohmann::json::object();
    std::optional<int> max_tokens;

    // The session id the compacted conversation belongs to, forwarded onto the
    // summarizer's ChatRequest so the compaction call sticks to the same
    // provider-routing key as the session's own traffic (OpenRouter
    // session_id). Emission still respects the same "parameters":
    // {"session_id": false} opt-out the rest of the session's requests honor
    // -- it rides in `parameters` above, whichever entry (the session's own or
    // small_model's) actually serves the call.
    std::string session_id;

    // The whole-context size going into this compaction: the backend-reported
    // occupancy of the last request (input + cache read/creation + output),
    // or 0 if none was reported. NOT the uncached input_tokens alone -- with
    // prompt caching that is a small fraction of the context.
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

// The production free-text compactor: summarizes `messages_to_compact` (the
// WHOLE working history -- the caller decides separately how much of it the
// summary replaces) into one synthetic message. The request is the session's
// own (system, tools, messages) plus one appended user message carrying
// `compression_prompt` verbatim.
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

// The shared request builder behind both compactors: `params`' system/tools,
// the history copied, `instruction` appended as a user message. `storage`
// owns the messages the returned request's span points into, and must
// outlive the provider call.
llm::ChatRequest BuildSummarizerRequest(std::span<const llm::Message> history,
                                        const CompactionRequest& params,
                                        std::string instruction,
                                        std::vector<llm::Message>& storage);

// Summarizes a message range into one synthetic message. Injected so the
// compaction path is testable without a live summarizer; production supplies
// compact_messages, which calls the provider with the preset's compression prompt.
// nullopt means "no compaction this cycle" -- failure degrades to no compaction,
// never to a lost transcript. Everything that varies is in CompactionRequest,
// supplied per call rather than bound at construction, because /agent can switch
// presets mid-session and binding would compact with the previous one's settings.
using Compactor = std::function<std::optional<CompactionResult>(
    std::span<const llm::Message> messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& request)>;

// Proposes a candidate cut index into `history` (messages before it would be
// summarized; messages at/after it stay verbatim) -- the OTHER half of the
// compaction seam (S4), independent of Compactor above. `keep_recent_tokens`
// is settings.json's memory.compaction_keep_recent_tokens, handed through so a
// strategy can honor it (W11: walk from the tail accumulating per-message
// estimates until the budget is met) without needing its own config plumbing.
//
// Core ALWAYS snaps whatever this returns backward to the nearest safe
// tool-call boundary (agent::snap_compaction_boundary) before using it -- a
// proposed cut is a starting point, never a guarantee, and that invariant is
// therefore not a contract a strategy author has to remember or can get wrong.
using CutSelector =
    std::function<std::size_t(const std::vector<llm::Message>& history,
                              std::int64_t keep_recent_tokens)>;

}  // namespace minimal::agent
