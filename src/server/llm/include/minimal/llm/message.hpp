#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::llm {

enum class Role { User, Assistant };

struct TextBlock {
    std::string text;
};

struct ThinkingBlock {
    std::string text;
    // Populated end-to-end by provider-anthropic from the stream's
    // signature_delta; REQUIRED to replay a thinking block to the API (an
    // unsigned block is dropped at request-build time rather than sent).
    std::optional<std::string> signature;
    // Anthropic's redacted_thinking payload: replayed verbatim, carries no
    // text and no signature. Mutually exclusive with signature by
    // construction (one block kind on the wire).
    std::optional<std::string> redacted_data = std::nullopt;
};

struct ToolUseBlock {
    std::string id, name;
    nlohmann::json input;
};

// Tool-result content is deliberately NOT the full recursive ContentBlock: a
// tool_result never itself carries a nested tool_use/tool_result on either the
// Anthropic or OpenAI wire format, so this stays a flat variant rather than
// needing indirection to break the recursion ARCHITECTURE.md's pseudocode implies.
using ToolResultContentBlock = std::variant<TextBlock>;

struct ToolResultBlock {
    std::string tool_use_id;
    std::vector<ToolResultContentBlock> content;
    bool is_error = false;
    // Client-only display payload (tools::ToolResult::display). Providers must
    // NEVER serialize it -- the model sees `content` only.
    nlohmann::json display = nullptr;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ThinkingBlock>;

struct Message {
    Role role;
    std::vector<ContentBlock> content;
};

struct Usage {
    int input_tokens = 0, output_tokens = 0;
    int cache_read_input_tokens = 0, cache_creation_input_tokens = 0;
};

// Concatenates every TextBlock in a content array, in order. Other block
// kinds (tool use/result, thinking) contribute nothing -- callers that need a
// broader byte count (e.g. token estimation) compute their own, since which
// kinds count is a policy decision this data-model header does not make.
inline std::string TextOf(const std::vector<ContentBlock>& content) {
    std::string out;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) out += text->text;
    }
    return out;
}

inline std::string TextOf(const Message& message) { return TextOf(message.content); }

// Late results of non-blocking subagents and background tasks. The spawn call
// was already answered (with its "started" ack), so the finished result can NOT
// ride a second tool_result for the same tool_use_id: the APIs require every
// tool_result to answer a tool_use in the immediately preceding assistant
// message, and reject the request otherwise. It arrives as a plain notification
// instead, tagged so it is never mistaken for something the user typed
// (agent::IsUserTextMessage ignores it: not a rewind target, not a steer, no wake).
inline constexpr std::string_view kTaskResultTag = "<task-result";

inline llm::Message MakeTaskResultMessage(const std::string& tool_use_id, bool is_error,
                                          const std::string& text) {
    std::string body = std::string(kTaskResultTag) + " tool_use_id=\"" + tool_use_id +
                       "\" status=\"" + (is_error ? "error" : "ok") + "\">\n" + text +
                       "\n</task-result>";
    return llm::Message{llm::Role::User, {llm::TextBlock{std::move(body)}}};
}

// A real human prompt (a steering submission, a rewind target, a turn ending
// with something still awaiting a reply) vs. a User-role message that only
// carries the model's own tool answers arriving late (a non-blocking
// subagent's completion, a background task's result). Role alone cannot
// distinguish the two -- both are role User -- so every caller that cares
// about "did a human actually say something here" shares this one check
// rather than reimplementing the role+non-empty-TextBlock scan.

}  // namespace minimal::llm
