#include "openai_request.hpp"

#include <variant>

namespace minimal::provider_openai {

namespace {

std::string JoinResultContent(const std::vector<llm::ToolResultContentBlock>& content) {
    std::string joined;
    for (const auto& block : content) {
        // ToolResultContentBlock is variant<TextBlock> only -- see message.hpp.
        joined += std::get<llm::TextBlock>(block).text;
    }
    return joined;
}

}  // namespace

nlohmann::json ToOpenAiMessages(const llm::ChatRequest& request) {
    nlohmann::json messages = nlohmann::json::array();
    if (request.system) {
        messages.push_back({{"role", "system"}, {"content", *request.system}});
    }

    for (const auto& message : request.messages) {
        std::string pending_text;
        nlohmann::json tool_calls = nlohmann::json::array();

        // Assumes the harness invariant (confirmed in agent_loop.cpp): a User
        // message is either all TextBlocks or all ToolResultBlocks, never
        // mixed, so labeling a flush as "user" here is always correct.
        auto flush_user_text = [&]() {
            if (!pending_text.empty()) {
                messages.push_back({{"role", "user"}, {"content", pending_text}});
                pending_text.clear();
            }
        };

        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                pending_text += text->text;
            } else if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                tool_calls.push_back({{"id", tool_use->id},
                                       {"type", "function"},
                                       {"function",
                                        {{"name", tool_use->name}, {"arguments", tool_use->input.dump()}}}});
            } else if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                flush_user_text();
                messages.push_back({{"role", "tool"},
                                     {"tool_call_id", tool_result->tool_use_id},
                                     {"content", JoinResultContent(tool_result->content)}});
            }
            // ThinkingBlock has no chat-completions equivalent -- dropped silently.
        }

        if (message.role == llm::Role::User) {
            flush_user_text();
        } else {
            nlohmann::json entry{{"role", "assistant"}};
            // OpenAI allows a null content when tool_calls carries the payload.
            entry["content"] =
                (pending_text.empty() && !tool_calls.empty()) ? nlohmann::json(nullptr) : nlohmann::json(pending_text);
            if (!tool_calls.empty()) entry["tool_calls"] = std::move(tool_calls);
            messages.push_back(std::move(entry));
        }
    }

    return messages;
}

nlohmann::json ToOpenAiTools(const std::vector<llm::ToolDefinition>& tools) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tool : tools) {
        out.push_back({{"type", "function"},
                        {"function",
                         {{"name", tool.name}, {"description", tool.description}, {"parameters", tool.input_schema}}}});
    }
    return out;
}

nlohmann::json ToOpenAiToolChoice(const llm::ToolChoiceSpec& choice) {
    switch (choice.kind) {
        case llm::ToolChoiceKind::Auto:
            return "auto";
        case llm::ToolChoiceKind::None:
            return "none";
        case llm::ToolChoiceKind::Required:
            return "required";
        case llm::ToolChoiceKind::Specific:
            return {{"type", "function"}, {"function", {{"name", *choice.tool_name}}}};
    }
    return "auto";
}

nlohmann::json BuildRequestBody(const llm::ChatRequest& request) {
    // parameters first -- everything below unconditionally overwrites same-named
    // keys, so a misconfigured `parameters` can never corrupt these mechanics.
    nlohmann::json body = request.parameters.is_object() ? request.parameters : nlohmann::json::object();

    body["model"] = request.model;
    body["messages"] = ToOpenAiMessages(request);
    body["stream"] = request.stream;

    if (!request.tools.empty()) {
        body["tools"] = ToOpenAiTools(request.tools);
        body["tool_choice"] = ToOpenAiToolChoice(request.tool_choice);
    } else {
        // OpenAI rejects tool_choice with no tools present.
        body.erase("tools");
        body.erase("tool_choice");
    }

    if (request.max_tokens) {
        body["max_tokens"] = *request.max_tokens;
    } else {
        body.erase("max_tokens");
    }

    if (request.stream) {
        // Needed to get a usage-bearing chunk at the end of the SSE stream.
        body["stream_options"] = {{"include_usage", true}};
    } else {
        body.erase("stream_options");
    }

    return body;
}

}  // namespace minimal::provider_openai
