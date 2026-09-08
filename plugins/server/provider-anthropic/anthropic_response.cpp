#include "anthropic_response.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace minimal::provider_anthropic {
namespace {

// Present-but-null discipline, same as provider-openai's SafeTokenCount:
// value() on a present-but-null key throws type_error, so only ABSENCE
// defaults to 0. is_number_integer also refuses floats.
int SafeTokenCount(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json.at(key).is_number_integer()) return 0;
    const std::int64_t value = json.at(key).get<std::int64_t>();
    if (value <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(value, std::numeric_limits<int>::max()));
}

}  // namespace

llm::StopReason MapStopReason(const std::string& stop_reason) {
    if (stop_reason == "end_turn") return llm::StopReason::EndTurn;
    if (stop_reason == "max_tokens") return llm::StopReason::MaxTokens;
    if (stop_reason == "stop_sequence") return llm::StopReason::StopSequence;
    if (stop_reason == "tool_use") return llm::StopReason::ToolUse;
    if (stop_reason == "refusal") return llm::StopReason::Refusal;
    // "pause_turn" is a server-tool (web search/code execution) continuation
    // signal -- this harness runs no server tools, so it should never arrive,
    // but a future backend sending it must degrade rather than crash.
    return llm::StopReason::Unknown;
}

llm::Usage ParseUsage(const nlohmann::json& usage_json) {
    llm::Usage usage;
    usage.input_tokens = SafeTokenCount(usage_json, "input_tokens");
    usage.output_tokens = SafeTokenCount(usage_json, "output_tokens");
    // Direct fields, unlike OpenAI's nested prompt_tokens_details.cached_tokens
    // (and OpenAI has no cache-write counterpart at all).
    usage.cache_read_input_tokens = SafeTokenCount(usage_json, "cache_read_input_tokens");
    usage.cache_creation_input_tokens = SafeTokenCount(usage_json, "cache_creation_input_tokens");
    return usage;
}

llm::ChatResponse ParseNonStreamingResponse(const nlohmann::json& body) {
    try {
        llm::ChatResponse response;
        response.id = body.contains("id") && !body.at("id").is_null()
                          ? body.at("id").get<std::string>()
                          : "";
        response.model = body.contains("model") && !body.at("model").is_null()
                             ? body.at("model").get<std::string>()
                             : "";

        if (!body.contains("content") || !body.at("content").is_array()) {
            throw AnthropicParseError("anthropic response has no content array");
        }
        for (const auto& block : body.at("content")) {
            const std::string type = block.value("type", "");
            if (type == "text") {
                std::string text = block.value("text", "");
                if (!text.empty()) response.content.push_back(llm::TextBlock{text});
            } else if (type == "thinking") {
                // Signed thinking is replayable; an unsigned block would be
                // dropped at request-build time, so it is emitted here with
                // whatever signature the body carried.
                llm::ThinkingBlock thinking;
                thinking.text = block.value("thinking", "");
                if (block.contains("signature") && block.at("signature").is_string()) {
                    thinking.signature = block.at("signature").get<std::string>();
                }
                response.content.push_back(std::move(thinking));
            } else if (type == "redacted_thinking") {
                llm::ThinkingBlock thinking;
                if (block.contains("data") && block.at("data").is_string()) {
                    thinking.redacted_data = block.at("data").get<std::string>();
                }
                response.content.push_back(std::move(thinking));
            } else if (type == "tool_use") {
                llm::ToolUseBlock tool_use;
                tool_use.id = block.at("id").get<std::string>();
                tool_use.name = block.at("name").get<std::string>();
                tool_use.input = block.contains("input") ? block.at("input") : nlohmann::json::object();
                response.content.push_back(std::move(tool_use));
            }
        }

        response.stop_reason =
            body.contains("stop_reason") && !body.at("stop_reason").is_null()
                ? MapStopReason(body.at("stop_reason").get<std::string>())
                : llm::StopReason::Unknown;
        if (body.contains("usage") && !body.at("usage").is_null()) {
            response.usage = ParseUsage(body.at("usage"));
        }

        return response;
    } catch (const AnthropicParseError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw AnthropicParseError(std::string("malformed anthropic response: ") + error.what());
    }
}

}  // namespace minimal::provider_anthropic
