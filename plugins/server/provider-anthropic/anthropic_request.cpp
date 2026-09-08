#include "anthropic_request.hpp"

#include <set>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace minimal::provider_anthropic {

namespace {

// Structural keys: (re)written from ChatRequest's own members below, so any
// `parameters` key of the same name is dropped rather than passed through --
// see anthropic_request.hpp's header comment and provider-openai's identical
// rule. `cache_ttl` is consumed here (it drives the system block's
// `cache_control.ttl`, below) and must never leak through as a bogus
// top-level Anthropic field.
const std::set<std::string_view>& StructuralKeys() {
    static const std::set<std::string_view> keys = {
        "model",  "messages", "stream", "tools",         "tool_choice",
        "system", "max_tokens", "cache_control", "cache_ttl"};
    return keys;
}

void AppendKey(std::string& out, std::string_view key) {
    out += '"';
    out += key;
    out += "\":";
}

void AppendStringValue(std::string& out, std::string_view value) {
    // nlohmann for the escaping only, not the structure -- O(this string's
    // length), never O(the history), same as provider-openai. The replace
    // error handler keeps invalid UTF-8 (e.g. a binary tool result) from
    // throwing type_error.316 out of the builder: it becomes U+FFFD instead.
    out += nlohmann::json(value).dump(-1, ' ', false,
                                      nlohmann::json::error_handler_t::replace);
}

std::string JoinResultContent(const std::vector<llm::ToolResultContentBlock>& content) {
    std::string joined;
    for (const auto& block : content) {
        // ToolResultContentBlock is variant<TextBlock> only -- see message.hpp.
        joined += std::get<llm::TextBlock>(block).text;
    }
    return joined;
}

std::string CacheTtl(const nlohmann::json& parameters) {
    if (parameters.is_object() && parameters.contains("cache_ttl") &&
        parameters.at("cache_ttl").is_string()) {
        const std::string ttl = parameters.at("cache_ttl").get<std::string>();
        // The API accepts exactly two values (README documents both); anything
        // else reaches the wire verbatim and fails there as an opaque 400.
        // Fail here with a name instead.
        if (ttl != "5m" && ttl != "1h") {
            throw std::invalid_argument(
                "cache_ttl must be \"5m\" or \"1h\" (got \"" + ttl + "\")");
        }
        return ttl;
    }
    return "";  // "" means the default 5-minute TTL: omit the "ttl" key entirely
}

}  // namespace

nlohmann::json ToAnthropicTools(const std::vector<llm::ToolDefinition>& tools) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tool : tools) {
        // No {"type":"function","function":{...}} wrapper: Anthropic's tool
        // object is {name, description, input_schema} directly, matching
        // llm::ToolDefinition almost verbatim.
        out.push_back({{"name", tool.name},
                       {"description", tool.description},
                       {"input_schema", tool.input_schema}});
    }
    return out;
}

nlohmann::json ToAnthropicToolChoice(const llm::ToolChoiceSpec& choice) {
    switch (choice.kind) {
        case llm::ToolChoiceKind::Auto:
            return nlohmann::json{{"type", "auto"}};
        case llm::ToolChoiceKind::None:
            return nlohmann::json{{"type", "none"}};
        case llm::ToolChoiceKind::Required:
            return nlohmann::json{{"type", "any"}};
        case llm::ToolChoiceKind::Specific:
            // "set iff kind == Specific" is the struct's own contract --
            // same unchecked-dereference guard as provider-openai's.
            if (!choice.tool_name) {
                throw std::invalid_argument(
                    "ToolChoiceKind::Specific requires tool_name to be set");
            }
            return nlohmann::json{{"type", "tool"}, {"name", *choice.tool_name}};
    }
    return nlohmann::json{{"type", "auto"}};
}

std::string BuildRequestBody(const llm::ChatRequest& request, bool stream) {
    if (!request.max_tokens) {
        throw std::invalid_argument(
            "anthropic requires max_tokens; set it on the model or provider in providers.json");
    }

    std::string out;
    out.reserve(4096);  // the history dominates; give the buffer a head start

    out += '{';
    bool first_key = true;

    // 1. parameters first, verbatim minus structural names.
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

    // 2. Structural fields, always written from ChatRequest's own members.
    if (!first_key) out += ',';
    first_key = false;
    AppendKey(out, "model");
    AppendStringValue(out, request.model);

    out += R"(,"max_tokens":)" + std::to_string(*request.max_tokens);

    if (request.system) {
        // A one-element block array, not a bare string, so it can carry a
        // cache_control breakpoint -- see the header comment. Tools render
        // before system on the wire, so this one marker caches both.
        out += R"(,"system":[{"type":"text","text":)";
        AppendStringValue(out, *request.system);
        out += R"(,"cache_control":{"type":"ephemeral")";
        if (const std::string ttl = CacheTtl(request.parameters); !ttl.empty()) {
            out += R"(,"ttl":)";
            AppendStringValue(out, ttl);
        }
        out += "}}]";
    }

    out += R"(,"messages":)";
    out += '[';
    // Consecutive llm::Messages of the SAME role are folded into one
    // Anthropic message (see the header comment: strict alternation is
    // enforced server-side, unlike OpenAI). One open "logical message" is
    // tracked at a time; role changes (or running out of messages) close it.
    // A message whose every block was dropped (an assistant turn consisting
    // only of UNSIGNED thinking blocks, or a zero-block message) is SKIPPED
    // ENTIRELY -- emitting "content":[] is an outright API rejection, and
    // closing the run around it would split its neighbors into consecutive
    // same-role wire messages. A turn of SIGNED thinking renders normally:
    // it is replayable content.
    bool first_wire_message = true;
    bool has_open_message = false;
    llm::Role open_role = llm::Role::User;
    std::string open_content;
    bool first_block_in_open = true;

    const auto close_open_message = [&] {
        if (!has_open_message) return;
        if (!first_wire_message) out += ',';
        first_wire_message = false;
        out += R"({"role":")";
        out += (open_role == llm::Role::User ? "user" : "assistant");
        out += R"(","content":[)";
        out += open_content;
        out += "]}";
        has_open_message = false;
        open_content.clear();
        first_block_in_open = true;
    };

    for (const auto& message : request.messages) {
        // Render THIS message's blocks into a local buffer first: only a
        // non-empty result joins (or opens) a logical run, so a fully-dropped
        // message is invisible to the folding and can never split a run or
        // emit an empty content array.
        std::string message_content;
        bool first_block_of_message = true;
        const auto append_block = [&](std::string block_json) {
            if (!first_block_of_message) message_content += ',';
            first_block_of_message = false;
            message_content += std::move(block_json);
        };
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                std::string item = R"({"type":"text","text":)";
                AppendStringValue(item, text->text);
                item += '}';
                append_block(std::move(item));
            } else if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                std::string item = R"({"type":"tool_use","id":)";
                AppendStringValue(item, tool_use->id);
                item += R"(,"name":)";
                AppendStringValue(item, tool_use->name);
                // Nested JSON object, not a stringified one -- Anthropic's
                // tool_use.input is a real object, unlike OpenAI's
                // tool_calls[].function.arguments string.
                item += R"(,"input":)";
                item += tool_use->input.dump();
                item += '}';
                append_block(std::move(item));
            } else if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                std::string item = R"({"type":"tool_result","tool_use_id":)";
                AppendStringValue(item, tool_result->tool_use_id);
                item += R"(,"content":)";
                AppendStringValue(item, JoinResultContent(tool_result->content));
                if (tool_result->is_error) item += R"(,"is_error":true)";
                item += '}';
                append_block(std::move(item));
            } else if (const auto* thinking = std::get_if<llm::ThinkingBlock>(&block)) {
                // Replayable forms only -- see the header comment. A redacted
                // block carries its data verbatim; a signed block round-trips
                // its signature; anything else stays dropped.
                std::string item;
                if (thinking->redacted_data) {
                    item = R"({"type":"redacted_thinking","data":)";
                    AppendStringValue(item, *thinking->redacted_data);
                    item += '}';
                    append_block(std::move(item));
                } else if (thinking->signature && !thinking->signature->empty()) {
                    item = R"({"type":"thinking","thinking":)";
                    AppendStringValue(item, thinking->text);
                    item += R"(,"signature":)";
                    AppendStringValue(item, *thinking->signature);
                    item += '}';
                    append_block(std::move(item));
                }
            }
        }
        if (message_content.empty()) continue;
        if (!has_open_message || open_role != message.role) {
            close_open_message();
            has_open_message = true;
            open_role = message.role;
            first_block_in_open = true;
        }
        if (!first_block_in_open) open_content += ',';
        first_block_in_open = false;
        open_content += message_content;
    }
    close_open_message();
    out += ']';

    out += R"(,"stream":)";
    out += stream ? "true" : "false";

    if (!request.tools.empty()) {
        out += R"(,"tools":)";
        out += ToAnthropicTools(request.tools).dump();
        out += R"(,"tool_choice":)";
        out += ToAnthropicToolChoice(request.tool_choice).dump();
    }

    // Top-level automatic caching for the growing conversation tail: the API
    // places this on the last cacheable block and moves it forward as
    // `messages` grows, which pairs with the explicit system-block breakpoint
    // above for agent loops (two of the four allowed breakpoints per request);
    // harmless (and simply unused) on the very first turn, when there is
    // nothing yet worth caching here. NOTE: this field's acceptance by the
    // live API is unverified from this repo -- if a gateway rejects it, this
    // is the line to remove.
    out += R"(,"cache_control":{"type":"ephemeral"})";

    out += '}';
    return out;
}

}  // namespace minimal::provider_anthropic
