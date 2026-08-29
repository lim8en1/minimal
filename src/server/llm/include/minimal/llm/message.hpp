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

}  // namespace minimal::llm
