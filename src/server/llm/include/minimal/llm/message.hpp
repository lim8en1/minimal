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
    std::optional<std::string> signature;  // reserved
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

}  // namespace minimal::llm
