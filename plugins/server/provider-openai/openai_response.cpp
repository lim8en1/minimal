#include "openai_response.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace minimal::provider_openai {
namespace {

// value(key, default) only defaults on ABSENCE -- present-but-null throws
// type_error. The streaming path guarded nulls (finish_reason, usage, ids);
// these helpers sweep the same discipline over the non-streaming parser and
// ParseUsage. is_number_integer also refuses floats: a fractional token count
// silently truncating into a valid-looking limit is the InheritNumber bug
// class (s12.27).
int SafeTokenCount(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json.at(key).is_number_integer()) return 0;
    const std::int64_t value = json.at(key).get<std::int64_t>();
    if (value <= 0) return 0;  // negative or zero: report nothing rather than poison the occupancy
    return static_cast<int>(std::min<std::int64_t>(value, std::numeric_limits<int>::max()));
}

}  // namespace

llm::StopReason MapFinishReason(const std::string& finish_reason) {
    if (finish_reason == "stop") return llm::StopReason::EndTurn;
    if (finish_reason == "length") return llm::StopReason::MaxTokens;
    if (finish_reason == "tool_calls" || finish_reason == "function_call") return llm::StopReason::ToolUse;
    if (finish_reason == "content_filter") return llm::StopReason::ContentFilter;
    if (finish_reason == "refusal") {
        // OpenAI's safety refusal on o-series/gpt-4o. The enum value existed,
        // had a wire mapping and a hook name -- and was produced by nothing:
        // refusals degraded to Unknown and surfaced as a generic truncation.
        return llm::StopReason::Refusal;
    }
    return llm::StopReason::Unknown;
}

llm::Usage ParseUsage(const nlohmann::json& usage_json) {
    llm::Usage usage;
    usage.input_tokens = SafeTokenCount(usage_json, "prompt_tokens");
    usage.output_tokens = SafeTokenCount(usage_json, "completion_tokens");

    // OpenAI-shape backends report cached prompt tokens under
    // prompt_tokens_details.cached_tokens (OpenAI itself, OpenRouter's
    // normalized accounting); DeepSeek's native API uses flat
    // prompt_cache_hit_tokens instead. Same quantity, two shapes.
    int cached = 0;
    if (usage_json.contains("prompt_tokens_details") && usage_json["prompt_tokens_details"].is_object()) {
        cached = SafeTokenCount(usage_json["prompt_tokens_details"], "cached_tokens");
    }
    if (cached == 0) {
        cached = SafeTokenCount(usage_json, "prompt_cache_hit_tokens");
    }
    usage.cache_read_input_tokens = cached;
    // prompt_tokens INCLUDES the cached tokens (OpenAI semantics); everything
    // downstream -- cost_model's input+cache_read pricing, cumulative input
    // totals -- assumes the Anthropic convention where input_tokens EXCLUDES
    // them. Subtract here, once, at the adapter boundary: cached prompt tokens
    // are billed at the cache-read rate, never twice (full rate + read rate).
    usage.input_tokens = std::max(0, usage.input_tokens - cached);
    // cache_creation_input_tokens has no OpenAI equivalent -- stays 0.
    return usage;
}

llm::ChatResponse ParseNonStreamingResponse(const nlohmann::json& body) {
    try {
        llm::ChatResponse response;
        // Same present-but-null discipline as everywhere else: `"id": null`
        // must degrade to empty, not kill the turn as a type_error.
        response.id = body.contains("id") && !body.at("id").is_null()
                          ? body.at("id").get<std::string>()
                          : "";
        response.model = body.contains("model") && !body.at("model").is_null()
                             ? body.at("model").get<std::string>()
                             : "";

        const auto& choices = body.at("choices");
        if (choices.empty()) throw OpenAiParseError("OpenAI response has no choices");
        const auto& choice = choices.at(0);
        const auto& message = choice.at("message");

        // Reasoning, two wire spellings -- "reasoning_content" (DeepSeek/GLM
        // and most OpenAI-compatible proxies) and "reasoning" (OpenRouter).
        // Captured as a ThinkingBlock ahead of the text, the position the
        // streaming path's blocks arrive in; unsigned, so the request
        // builder drops it from replay history.
        for (const char* key : {"reasoning_content", "reasoning"}) {
            if (message.contains(key) && message.at(key).is_string()) {
                std::string reasoning = message.at(key).get<std::string>();
                if (!reasoning.empty()) {
                    response.content.push_back(
                        llm::ThinkingBlock{std::move(reasoning), std::nullopt, std::nullopt});
                }
                break;   // first spelling present wins; never both
            }
        }

        if (message.contains("content") && !message.at("content").is_null()) {
            std::string text = message.at("content").get<std::string>();
            if (!text.empty()) response.content.push_back(llm::TextBlock{text});
        }

        if (message.contains("tool_calls")) {
            for (const auto& tool_call : message.at("tool_calls")) {
                llm::ToolUseBlock block;
                block.id = tool_call.at("id").get<std::string>();
                block.name = tool_call.at("function").at("name").get<std::string>();
                std::string arguments = tool_call.at("function").at("arguments").get<std::string>();
                block.input = arguments.empty() ? nlohmann::json::object() : nlohmann::json::parse(arguments);
                response.content.push_back(std::move(block));
            }
        }

        response.stop_reason =
            choice.contains("finish_reason") && !choice.at("finish_reason").is_null()
                ? MapFinishReason(choice.at("finish_reason").get<std::string>())
                : llm::StopReason::Unknown;
        if (body.contains("usage") && !body.at("usage").is_null()) {
            response.usage = ParseUsage(body.at("usage"));
        }

        return response;
    } catch (const OpenAiParseError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw OpenAiParseError(std::string("malformed OpenAI response: ") + error.what());
    }
}

}  // namespace minimal::provider_openai
