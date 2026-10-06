#include "minimal/agent/context_compactor.hpp"

#include <cctype>
#include <string>
#include <variant>

#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/agent/policy.hpp"

namespace minimal::agent {

// Serialized size of a JSON value, without serializing it. Structural
// characters are counted the way dump() would emit them; the point is a stable
// approximation, not byte equality with a string nobody keeps.
std::int64_t JsonSizeEstimate(const nlohmann::json& value) {
    switch (value.type()) {
        case nlohmann::json::value_t::string:
            return static_cast<std::int64_t>(value.get_ref<const std::string&>().size()) + 2;
        case nlohmann::json::value_t::object: {
            std::int64_t total = 2;  // {}
            for (const auto& [key, child] : value.items()) {
                total += static_cast<std::int64_t>(key.size()) + 4;  // "key":,
                total += JsonSizeEstimate(child);
            }
            return total;
        }
        case nlohmann::json::value_t::array: {
            std::int64_t total = 2;  // []
            for (const auto& child : value) total += JsonSizeEstimate(child) + 1;
            return total;
        }
        case nlohmann::json::value_t::null:
            return static_cast<std::int64_t>(std::string_view("null").size());
        case nlohmann::json::value_t::boolean:
            return static_cast<std::int64_t>(
                std::string_view(value.get<bool>() ? "true" : "false").size());
        default:
            return kEstimatedJsonNumberBytes;  // numbers, binary
    }
}

std::int64_t estimate_tokens(std::span<const llm::Message> messages) {
    return estimate_bytes(messages) / kEstimatedBytesPerToken;
}

std::int64_t estimate_bytes(std::span<const llm::Message> messages) {
    std::int64_t bytes = 0;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                bytes += static_cast<std::int64_t>(text->text.size());
            } else if (const auto* thinking = std::get_if<llm::ThinkingBlock>(&block)) {
                bytes += static_cast<std::int64_t>(thinking->text.size());
            } else if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) {
                // Sized, not serialized. dump() built the entire tool input as a
                // heap string purely to read .size() and throw it away -- over
                // the WHOLE transcript, on the resume path, where this runs to
                // seed the compaction trigger. The estimate is bytes/4 either
                // way, so an approximation of the serialized size is exactly as
                // good as the real thing.
                bytes += static_cast<std::int64_t>(use->name.size()) + JsonSizeEstimate(use->input);
            } else if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                for (const auto& item : result->content) {
                    if (const auto* text = std::get_if<llm::TextBlock>(&item)) {
                        bytes += static_cast<std::int64_t>(text->text.size());
                    }
                }
            }
        }
    }
    return bytes;
}

llm::ChatRequest BuildSummarizerRequest(std::span<const llm::Message> history,
                                        const CompactionRequest& params,
                                        std::string instruction,
                                        std::vector<llm::Message>& storage) {
    // A copy, once per compaction: the span contract (chat.hpp) needs one
    // contiguous range, and the appended instruction cannot go into the
    // loop's own history_. Byte-identical to what the main loop sent, so the
    // provider's prefix cache covers everything before the instruction.
    storage.assign(history.begin(), history.end());
    storage.push_back(llm::Message{llm::Role::User, {llm::TextBlock{std::move(instruction)}}});

    llm::ChatRequest request;
    request.system = params.system;
    request.tools = params.tools;
    request.messages = storage;
    request.model = std::string(params.model_name);
    request.stream = true;
    // The preset's, not defaults. A summarization call running with different
    // sampling than the conversation it summarizes is a silent inconsistency
    // -- and a cache miss -- and an absent max_tokens is worse than
    // inconsistent -- see the header.
    request.parameters = params.parameters;
    request.max_tokens = params.max_tokens;
    // Same routing key as the conversation being summarized -- whether the
    // call is served by the session's own provider or small_model, emission
    // is decided by the parameters riding this request (see chat.hpp).
    request.session_id = params.session_id;
    request.should_abort = params.is_cancelled;
    return request;
}

std::optional<CompactionResult> compact_messages(
    std::span<const llm::Message> messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& params) {
    const common::Logger& logger = params.logger ? *params.logger : common::DefaultLogger();
    // Same "session <id>: " shape the agent loop's adjacent compaction lines
    // use, so the two interleaved in one log file read as one stream.
    const std::string where = params.session_label.empty()
                                  ? std::string{}
                                  : "session " + std::string(params.session_label) + ": ";

    // No prompt, no compaction: a summarization request with no instructions would
    // burn tokens to produce garbage. An empty prompt means prompts/compression.md
    // is missing from the config dir.
    if (IsBlank(params.compression_prompt)) {
        logger.error("{}compaction skipped: no compression prompt configured", where);
        return std::nullopt;
    }

    std::vector<llm::Message> storage;
    const llm::ChatRequest request = BuildSummarizerRequest(
        messages_to_compact, params, std::string(params.compression_prompt), storage);

    llm::ChatResponse response;
    try {
        // StreamComplete rather than Complete so the sink can abort on
        // cancellation: a summarization call over half of a 512k-token context
        // must not stall a CancelTurn until request_timeout. The sink swallows
        // events -- the summary is internal and never reaches the client as
        // stream traffic -- and polls the cancel flag instead.
        response = provider.StreamComplete(request, [&params](const llm::StreamEvent&) {
            return !params.is_cancelled || !params.is_cancelled();
        });
    } catch (const llm::LlmError& error) {
        // The trigger is still armed: occupancy did not drop, so the next turn
        // retries. Logging and moving on is the whole policy -- a broken
        // summarizer must never take the transcript down with it.
        logger.warn("{}compaction failed (will retry next turn): {}", where, error.what());
        return std::nullopt;
    }

    if (response.stop_reason == llm::StopReason::Cancelled) {
        return std::nullopt;
    }

    // A summary that hit the output cap is cut off mid-sentence, and accepting
    // it would permanently replace half the conversation with a fragment whose
    // only other copy is in SQLite where the loop cannot reach it. Same rule the
    // agent loop applies to MaxTokens on a normal turn, for a worse consequence.
    if (response.stop_reason == llm::StopReason::MaxTokens) {
        logger.error(
            "{}compaction produced a truncated summary (hit max_tokens); skipping -- raise "
            "max_tokens for this provider or shorten the compaction prompt",
            where);
        return std::nullopt;
    }

    const std::string text = llm::TextOf(response.content);
    if (IsBlank(text)) {
        logger.error("{}compaction produced no usable text; skipping", where);
        return std::nullopt;
    }

    CompactionResult result;
    // User role: the summary stands in for the compacted prefix of the
    // conversation, and a user message is the shape both wire formats accept at
    // the head of a history.
    result.summary_message =
        llm::Message{llm::Role::User, {llm::TextBlock{std::string(kSummaryFraming) + text}}};
    // Both branches measure the WHOLE context, so the number means one thing.
    // Estimating only `messages_to_compact` here -- as this used to -- made the
    // fallback and the ground truth incomparable.
    result.context_tokens_before =
        params.last_reported_input_tokens > 0 ? params.last_reported_input_tokens
        : params.full_history                 ? estimate_tokens(*params.full_history)
                                              : estimate_tokens(messages_to_compact);
    return result;
}

}  // namespace minimal::agent
