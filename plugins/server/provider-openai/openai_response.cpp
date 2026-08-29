#include "openai_response.hpp"

namespace minimal::provider_openai {

llm::StopReason MapFinishReason(const std::string& finish_reason) {
    if (finish_reason == "stop") return llm::StopReason::EndTurn;
    if (finish_reason == "length") return llm::StopReason::MaxTokens;
    if (finish_reason == "tool_calls" || finish_reason == "function_call") return llm::StopReason::ToolUse;
    if (finish_reason == "content_filter") return llm::StopReason::ContentFilter;
    return llm::StopReason::Unknown;
}

llm::Usage ParseUsage(const nlohmann::json& usage_json) {
    llm::Usage usage;
    usage.input_tokens = usage_json.value("prompt_tokens", 0);
    usage.output_tokens = usage_json.value("completion_tokens", 0);
    if (usage_json.contains("prompt_tokens_details") && usage_json["prompt_tokens_details"].contains("cached_tokens")) {
        usage.cache_read_input_tokens = usage_json["prompt_tokens_details"]["cached_tokens"].get<int>();
    }
    // cache_creation_input_tokens has no OpenAI equivalent -- stays 0.
    return usage;
}

llm::ChatResponse ParseNonStreamingResponse(const nlohmann::json& body) {
    try {
        llm::ChatResponse response;
        response.id = body.value("id", "");
        response.model = body.value("model", "");

        const auto& choices = body.at("choices");
        if (choices.empty()) throw OpenAiParseError("OpenAI response has no choices");
        const auto& choice = choices.at(0);
        const auto& message = choice.at("message");

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

        response.stop_reason = MapFinishReason(choice.value("finish_reason", ""));
        if (body.contains("usage")) response.usage = ParseUsage(body.at("usage"));

        return response;
    } catch (const OpenAiParseError&) {
        throw;
    } catch (const nlohmann::json::exception& error) {
        throw OpenAiParseError(std::string("malformed OpenAI response: ") + error.what());
    }
}

}  // namespace minimal::provider_openai
