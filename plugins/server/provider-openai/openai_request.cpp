#include "openai_request.hpp"

#include <set>
#include <string_view>
#include <variant>

namespace minimal::provider_openai {

namespace {

// Structural field names: these are (re)written from ChatRequest's own members
// below, and any `parameters` key of the same name is DROPPED rather than
// emitted after them. That preserves the merge-order guarantee exactly without
// emitting duplicate JSON keys: the previous implementation built a nlohmann
// object (last write wins); a string builder that merely appended both would
// leave precedence to the receiving parser, which is not a guarantee.
const std::set<std::string_view>& StructuralKeys() {
    static const std::set<std::string_view> keys = {"model",    "messages",   "stream",
                                                    "tools",    "tool_choice", "max_tokens",
                                                    "stream_options"};
    return keys;
}

void AppendKey(std::string& out, std::string_view key) {
    out += '"';
    out += key;
    out += "\":";
}

void AppendStringValue(std::string& out, std::string_view value) {
    // nlohmann for the escaping, not the structure: a string's serialized form
    // is O(its own length), never O(the history).
    out += nlohmann::json(value).dump();
}

}  // namespace

std::string JoinResultContent(const std::vector<llm::ToolResultContentBlock>& content) {
    std::string joined;
    for (const auto& block : content) {
        // ToolResultContentBlock is variant<TextBlock> only -- see message.hpp.
        joined += std::get<llm::TextBlock>(block).text;
    }
    return joined;
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

std::string BuildRequestBody(const llm::ChatRequest& request) {
    // THE WHOLE BODY IS SERIALIZED INCREMENTALLY. The previous version built a
    // nlohmann DOM of the entire history and then dump()ed it -- two
    // materializations of every message per provider call, against a stated
    // 512k-1M token goal. Only O(1)-sized pieces (parameters values, tool
    // definitions) go through nlohmann here; message text is escaped
    // straight into the output buffer.
    std::string out;
    // The history dominates the body; give the buffer a head start so the
    // first few appends do not re-grow it. 4x bytes-per-token estimate over
    // the request's own serialized size is not knowable cheaply, so this is a
    // floor, not a prediction.
    out.reserve(4096);

    out += '{';
    bool first_key = true;

    // 1. parameters first, verbatim minus structural names -- see the comment
    //    on StructuralKeys() for why "dropped" preserves the old guarantee.
    if (request.parameters.is_object()) {
        for (const auto& [key, value] : request.parameters.items()) {
            if (StructuralKeys().count(key) != 0) continue;
            if (!first_key) out += ',';
            first_key = false;
            AppendStringValue(out, key);
            out += ':';
            out += value.dump();
        }
    }

    // 2. Structural fields, always written from ChatRequest's own members --
    //    a misconfigured `parameters` can tweak generation, never mechanics.
    //    (Every key from here on carries its own leading comma.)
    if (!first_key) out += ',';
    first_key = false;
    AppendKey(out, "model");
    AppendStringValue(out, request.model);

    out += R"(,"messages":)";
    out += '[';
    // One flag for the WHOLE array: a single llm::Message can serialize into
    // SEVERAL wire messages (a user text flush plus one tool message per
    // result), so the flag is shared by every emission site, not per iteration.
    bool first_message = true;
    const auto emit_message = [&](std::string serialized) {
        if (!first_message) out += ',';
        first_message = false;
        out += std::move(serialized);
    };

    if (request.system) {
        std::string system = R"({"role":"system","content":)";
        AppendStringValue(system, *request.system);
        system += '}';
        emit_message(std::move(system));
    }

    for (const auto& message : request.messages) {
        std::string pending_text;
        std::string tool_calls;  // serialized incrementally alongside the walk
        bool first_tool_call = true;

        // Assumes the harness invariant (confirmed in agent_loop.cpp): a User
        // message is either all TextBlocks or all ToolResultBlocks, never
        // mixed, so labeling a flush as "user" here is always correct.
        const auto flush_user_text = [&]() {
            if (!pending_text.empty()) {
                std::string user = R"({"role":"user","content":)";
                AppendStringValue(user, pending_text);
                user += '}';
                emit_message(std::move(user));
                pending_text.clear();
            }
        };

        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                pending_text += text->text;
            } else if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                if (!first_tool_call) tool_calls += ',';
                first_tool_call = false;
                tool_calls += R"({"id":)";
                AppendStringValue(tool_calls, tool_use->id);
                tool_calls += R"(,"type":"function","function":{"name":)";
                AppendStringValue(tool_calls, tool_use->name);
                // OpenAI's contract: arguments is a STRING containing JSON,
                // not a nested object. Escape the dump accordingly.
                tool_calls += R"(,"arguments":)";
                AppendStringValue(tool_calls, tool_use->input.dump());
                tool_calls += "}}";
            } else if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                flush_user_text();
                std::string tool = R"({"role":"tool","tool_call_id":)";
                AppendStringValue(tool, tool_result->tool_use_id);
                tool += R"(,"content":)";
                AppendStringValue(tool, JoinResultContent(tool_result->content));
                tool += '}';
                emit_message(std::move(tool));
            }
            // ThinkingBlock has no chat-completions equivalent -- dropped silently.
        }

        if (message.role == llm::Role::User) {
            flush_user_text();
        } else {
            std::string assistant = R"({"role":"assistant","content":)";
            // OpenAI allows a null content when tool_calls carries the payload.
            if (pending_text.empty() && !first_tool_call) {
                assistant += "null";
            } else {
                AppendStringValue(assistant, pending_text);
            }
            if (!first_tool_call) {
                assistant += R"(,"tool_calls":[)";
                assistant += tool_calls;
                assistant += ']';
            }
            assistant += '}';
            emit_message(std::move(assistant));
        }
    }
    out += ']';

    out += R"(,"stream":)";
    out += request.stream ? "true" : "false";

    if (!request.tools.empty()) {
        out += R"(,"tools":)";
        out += ToOpenAiTools(request.tools).dump();
        out += R"(,"tool_choice":)";
        out += ToOpenAiToolChoice(request.tool_choice).dump();
    }

    if (request.max_tokens) {
        out += R"(,"max_tokens":)" + std::to_string(*request.max_tokens);
    }

    if (request.stream) {
        // Needed to get a usage-bearing chunk at the end of the SSE stream.
        out += R"(,"stream_options":{"include_usage":true})";
    }

    out += '}';
    return out;
}

}  // namespace minimal::provider_openai
