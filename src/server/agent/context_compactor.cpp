#include "minimal/agent/context_compactor.hpp"

#include <cctype>
#include <string>
#include <variant>

#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"

namespace minimal::agent {
namespace {

constexpr std::string_view kSummaryFraming =
    "The earlier part of this conversation was compacted into the following summary.\n\n";

// Concatenates every TextBlock in a content array -- the only kind a
// summarization response is expected to contain.
std::string TextOf(const std::vector<llm::ContentBlock>& content) {
    std::string out;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
            out += text->text;
        }
    }
    return out;
}

// Rough size of a message range in tokens: serialized bytes / 4. Used only when
// the backend reported no usage at all, where even a rough number is better than
// nothing for "how much did compaction save".
std::int64_t EstimateTokens(const std::vector<llm::Message>& messages) {
    std::int64_t bytes = 0;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                bytes += static_cast<std::int64_t>(text->text.size());
            } else if (const auto* thinking = std::get_if<llm::ThinkingBlock>(&block)) {
                bytes += static_cast<std::int64_t>(thinking->text.size());
            } else if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) {
                bytes += static_cast<std::int64_t>(use->name.size() + use->input.dump().size());
            } else if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                for (const auto& item : result->content) {
                    if (const auto* text = std::get_if<llm::TextBlock>(&item)) {
                        bytes += static_cast<std::int64_t>(text->text.size());
                    }
                }
            }
        }
    }
    return bytes / 4;
}

bool IsBlank(std::string_view text) {
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

std::optional<CompactionResult> compact_messages(
    const std::vector<llm::Message>& messages_to_compact, llm::LlmProvider& provider,
    std::string_view model_name, std::string_view compression_prompt,
    std::int64_t last_reported_input_tokens, std::function<bool()> is_cancelled) {
    const auto& logger = common::DefaultLogger();

    // No prompt, no compaction: a summarization request with no instructions would
    // burn tokens to produce garbage. An empty prompt means the bundled
    // prompts/compression.md was never installed into the config dir.
    if (IsBlank(compression_prompt)) {
        logger.error("compaction skipped: no compression prompt configured");
        return std::nullopt;
    }

    llm::ChatRequest request;
    request.system = std::string(compression_prompt);
    request.messages = messages_to_compact;
    request.model = std::string(model_name);
    request.stream = true;

    llm::ChatResponse response;
    try {
        // StreamComplete rather than Complete so the sink can abort on
        // cancellation: a summarization call over half of a 512k-token context
        // must not stall a CancelTurn until request_timeout. The sink swallows
        // events -- the summary is internal and never reaches the client as
        // stream traffic -- and polls the cancel flag instead.
        response = provider.StreamComplete(request, [&is_cancelled](const llm::StreamEvent&) {
            return !is_cancelled || !is_cancelled();
        });
    } catch (const llm::LlmError& error) {
        // The trigger is still armed: occupancy did not drop, so the next turn
        // retries. Logging and moving on is the whole policy -- a broken
        // summarizer must never take the transcript down with it.
        logger.warn("compaction failed (will retry next turn): {}", error.what());
        return std::nullopt;
    }

    if (response.stop_reason == llm::StopReason::Cancelled) {
        return std::nullopt;
    }

    const std::string text = TextOf(response.content);
    if (IsBlank(text)) {
        logger.error("compaction produced no usable text; skipping");
        return std::nullopt;
    }

    CompactionResult result;
    // User role: the summary stands in for the compacted prefix of the
    // conversation, and a user message is the shape both wire formats accept at
    // the head of a history.
    result.summary_message =
        llm::Message{llm::Role::User, {llm::TextBlock{std::string(kSummaryFraming) + text}}};
    result.original_token_count = last_reported_input_tokens > 0
                                      ? last_reported_input_tokens
                                      : EstimateTokens(messages_to_compact);
    return result;
}

}  // namespace minimal::agent
