#include "openai_request.hpp"

#include <set>
#include <stdexcept>
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
// "cache_control" / "cache_ttl" are consumed by the builder (see
// CacheControlEnabled / CacheTtl) and must never leak onto the wire, same rule
// as provider-anthropic's treatment of "cache_ttl".
const std::set<std::string_view>& StructuralKeys() {
    static const std::set<std::string_view> keys = {"model",    "messages",   "stream",
                                                    "tools",    "tool_choice", "max_tokens",
                                                    "stream_options", "cache_control",
                                                    "cache_ttl"};
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

// OpenRouter routes OpenAI-shaped requests to backends that cache the way
// Anthropic does -- and Anthropic-model caching through OpenRouter is only
// reachable by marking message content parts with `cache_control`. The flag is
// OPT-IN per provider/model in providers.json ("parameters": {"cache_control":
// true}) because plain OpenAI, llama.cpp and vLLM cache prompts automatically
// (or reject unknown content-part fields outright); off, the builder's output
// is byte-identical to what it has always produced.
bool CacheControlEnabled(const nlohmann::json& parameters) {
    return parameters.is_object() && parameters.contains("cache_control") &&
           parameters.at("cache_control").is_boolean() &&
           parameters.at("cache_control").get<bool>();
}

// Same key provider-anthropic consumes, for parity: "5m" (the default, key
// omitted) or "1h". Only meaningful when CacheControlEnabled is true.
std::string CacheTtl(const nlohmann::json& parameters) {
    if (parameters.is_object() && parameters.contains("cache_ttl") &&
        parameters.at("cache_ttl").is_string()) {
        return parameters.at("cache_ttl").get<std::string>();
    }
    return "";
}

// One wire message under construction. Content is split out of the shell so
// the tail-breakpoint decision (below) can upgrade a plain string content into
// a marked content-part array WITHOUT string surgery on already-serialized
// JSON -- the wire message is only assembled at flush time.
struct WireMessage {
    std::string head;   // '{"role":...,"content":' (tool messages: + tool_call_id)
    std::string content;  // quoted-escaped string, "null", or a pre-built part array
    bool content_is_string = false;  // upgradeable to a marked part array
    std::string tail;   // tool_calls / closing brace
};

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
            // "set iff kind == Specific" is the struct's contract; an unchecked
            // dereference here was UB on the one violation path, from public,
            // individually-tested plugin API.
            if (!choice.tool_name) {
                throw std::invalid_argument(
                    "ToolChoiceKind::Specific requires tool_name to be set");
            }
            return {{"type", "function"}, {"function", {{"name", *choice.tool_name}}}};
    }
    return "auto";
}

std::string BuildRequestBody(const llm::ChatRequest& request, bool stream) {
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
    //
    // THE LAST WIRE MESSAGE IS HELD, not written immediately: when cache_control
    // is on, its content must become a marked part array (the conversation-tail
    // breakpoint -- the OpenAI-shaped equivalent of provider-anthropic's
    // auto-placed top-level cache_control), and which message is last is only
    // known once the walk is over. Every earlier message flushes verbatim, so
    // the flag-off body and the flag-on prefix stay byte-stable -- the cached
    // prefix is never disturbed turn to turn.
    const bool caching = CacheControlEnabled(request.parameters);
    const std::string ttl = CacheTtl(request.parameters);

    const auto write_message = [&](const WireMessage& message, bool tail_breakpoint) {
        out += message.head;
        if (message.content_is_string && tail_breakpoint) {
            out += R"([{"type":"text","text":)";
            out += message.content;
            out += R"(,"cache_control":{"type":"ephemeral")";
            if (!ttl.empty()) {
                out += R"(,"ttl":)";
                out += nlohmann::json(ttl).dump();
            }
            out += "}}]";
        } else {
            out += message.content;
        }
        out += message.tail;
    };

    bool first_message = true;
    WireMessage held;
    bool have_held = false;
    const auto emit_message = [&](WireMessage message) {
        if (have_held) {
            if (!first_message) out += ',';
            first_message = false;
            write_message(held, /*tail_breakpoint=*/false);
        }
        held = std::move(message);
        have_held = true;
    };

    if (request.system) {
        WireMessage system;
        system.head = R"({"role":"system","content":)";
        if (caching) {
            // A part array, not a bare string, so it can carry a cache_control
            // breakpoint: tools render before system on the wire, so this one
            // marker caches both -- the same tools+system strategy as
            // provider-anthropic's system block.
            system.head += R"([{"type":"text","text":)";
            AppendStringValue(system.head, *request.system);
            system.head += R"(,"cache_control":{"type":"ephemeral")";
            if (!ttl.empty()) {
                system.head += R"(,"ttl":)";
                system.head += nlohmann::json(ttl).dump();
            }
            system.head += "}}]";
        } else {
            AppendStringValue(system.head, *request.system);
        }
        system.tail = "}";
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
                WireMessage user;
                user.head = R"({"role":"user","content":)";
                user.content_is_string = true;
                AppendStringValue(user.content, pending_text);
                user.tail = "}";
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
                WireMessage tool;
                tool.head = R"({"role":"tool","tool_call_id":)";
                AppendStringValue(tool.head, tool_result->tool_use_id);
                tool.head += R"(,"content":)";
                tool.content_is_string = true;
                AppendStringValue(tool.content, JoinResultContent(tool_result->content));
                tool.tail = "}";
                emit_message(std::move(tool));
            }
            // ThinkingBlock has no chat-completions equivalent -- dropped silently.
        }

        if (message.role == llm::Role::User) {
            flush_user_text();
        } else {
            WireMessage assistant;
            assistant.head = R"({"role":"assistant","content":)";
            // OpenAI allows a null content when tool_calls carries the payload.
            if (pending_text.empty() && !first_tool_call) {
                assistant.content = "null";  // nothing to hang a breakpoint on
            } else {
                assistant.content_is_string = true;
                AppendStringValue(assistant.content, pending_text);
            }
            if (!first_tool_call) {
                assistant.tail += R"(,"tool_calls":[)";
                assistant.tail += tool_calls;
                assistant.tail += ']';
            }
            assistant.tail += '}';
            emit_message(std::move(assistant));
        }
    }

    // The held message is the last wire message: the tail breakpoint lands on
    // it when caching is on (and on nothing when it is a pre-marked system
    // message or a content-less assistant tool_call message).
    if (have_held) {
        if (!first_message) out += ',';
        first_message = false;
        write_message(held, caching);
    }
    out += ']';

    out += R"(,"stream":)";
    out += stream ? "true" : "false";

    if (!request.tools.empty()) {
        out += R"(,"tools":)";
        out += ToOpenAiTools(request.tools).dump();
        out += R"(,"tool_choice":)";
        out += ToOpenAiToolChoice(request.tool_choice).dump();
    }

    if (request.max_tokens) {
        out += R"(,"max_tokens":)" + std::to_string(*request.max_tokens);
    }

    if (stream) {
        // Needed to get a usage-bearing chunk at the end of the SSE stream.
        out += R"(,"stream_options":{"include_usage":true})";
    }

    out += '}';
    return out;
}

}  // namespace minimal::provider_openai
