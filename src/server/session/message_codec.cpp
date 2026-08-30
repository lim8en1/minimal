#include "minimal/session/message_codec.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace minimal::session {
namespace {

nlohmann::json TextBlockToJson(const llm::TextBlock& b) {
    return {{"type", "text"}, {"text", b.text}};
}

nlohmann::json BlockToJson(const llm::ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> nlohmann::json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, llm::TextBlock>) {
                return TextBlockToJson(b);
            } else if constexpr (std::is_same_v<T, llm::ThinkingBlock>) {
                nlohmann::json j = {{"type", "thinking"}, {"text", b.text}};
                j["signature"] = b.signature ? nlohmann::json(*b.signature) : nlohmann::json(nullptr);
                return j;
            } else if constexpr (std::is_same_v<T, llm::ToolUseBlock>) {
                // input is arbitrary JSON and is preserved verbatim.
                return {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
            } else {  // ToolResultBlock
                nlohmann::json content = nlohmann::json::array();
                for (const auto& inner : b.content) {
                    if (const auto* text = std::get_if<llm::TextBlock>(&inner)) {
                        content.push_back(TextBlockToJson(*text));
                    }
                }
                return {{"type", "tool_result"}, {"tool_use_id", b.tool_use_id},
                        {"content", std::move(content)}, {"is_error", b.is_error}};
            }
        },
        block);
}

llm::ContentBlock BlockFromJson(const nlohmann::json& j, const std::string& ctx) {
    if (!j.is_object() || !j.contains("type") || !j.at("type").is_string()) {
        throw std::runtime_error(ctx + ": block is not an object with a string 'type'");
    }
    const std::string type = j.at("type").get<std::string>();

    if (type == "text") {
        if (!j.contains("text") || !j.at("text").is_string()) {
            throw std::runtime_error(ctx + ": text block missing string 'text'");
        }
        return llm::TextBlock{j.at("text").get<std::string>()};
    }

    if (type == "thinking") {
        if (!j.contains("text") || !j.at("text").is_string()) {
            throw std::runtime_error(ctx + ": thinking block missing string 'text'");
        }
        llm::ThinkingBlock b;
        b.text = j.at("text").get<std::string>();
        if (j.contains("signature") && !j.at("signature").is_null()) {
            if (!j.at("signature").is_string()) {
                throw std::runtime_error(ctx + ": thinking block 'signature' must be a string or null");
            }
            b.signature = j.at("signature").get<std::string>();
        }
        return b;
    }

    if (type == "tool_use") {
        llm::ToolUseBlock b;
        for (const char* key : {"id", "name"}) {
            if (!j.contains(key) || !j.at(key).is_string()) {
                throw std::runtime_error(std::string(ctx) + ": tool_use block missing string '" + key + "'");
            }
        }
        b.id = j.at("id").get<std::string>();
        b.name = j.at("name").get<std::string>();
        if (!j.contains("input")) {
            throw std::runtime_error(ctx + ": tool_use block missing 'input'");
        }
        b.input = j.at("input");
        return b;
    }

    if (type == "tool_result") {
        if (!j.contains("tool_use_id") || !j.at("tool_use_id").is_string()) {
            throw std::runtime_error(ctx + ": tool_result block missing string 'tool_use_id'");
        }
        llm::ToolResultBlock b;
        b.tool_use_id = j.at("tool_use_id").get<std::string>();
        if (j.contains("is_error")) {
            if (!j.at("is_error").is_boolean()) {
                throw std::runtime_error(ctx + ": tool_result block 'is_error' must be a boolean");
            }
            b.is_error = j.at("is_error").get<bool>();
        }
        if (j.contains("content") && j.at("content").is_array()) {
            int i = 0;
            for (const auto& inner : j.at("content")) {
                const llm::ContentBlock block =
                    BlockFromJson(inner, ctx + ".content[" + std::to_string(i++) + "]");
                if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                    b.content.emplace_back(*text);
                } else {
                    // The C++ model is a flat variant for exactly this reason:
                    // tool results never nest tool_use/tool_result on either wire.
                    throw std::runtime_error(ctx + ": tool_result content may only hold text blocks");
                }
            }
        }
        return b;
    }

    throw std::runtime_error(ctx + ": unknown block type '" + type + "'");
}

}  // namespace

nlohmann::json MessageToJson(const llm::Message& message) {
    nlohmann::json doc;
    doc["role"] = message.role == llm::Role::User ? "user" : "assistant";
    nlohmann::json content = nlohmann::json::array();
    for (const auto& block : message.content) {
        content.push_back(BlockToJson(block));
    }
    doc["content"] = std::move(content);
    return doc;
}

llm::Message JsonToMessage(const nlohmann::json& doc) {
    try {
        if (!doc.is_object()) throw std::runtime_error("message is not an object");
        if (!doc.contains("role") || !doc.at("role").is_string()) {
            throw std::runtime_error("message missing string 'role'");
        }
        llm::Role role;
        if (doc.at("role") == "user") {
            role = llm::Role::User;
        } else if (doc.at("role") == "assistant") {
            role = llm::Role::Assistant;
        } else {
            throw std::runtime_error("message has unknown role '" + doc.at("role").get<std::string>() + "'");
        }

        if (!doc.contains("content") || !doc.at("content").is_array()) {
            throw std::runtime_error("message missing array 'content'");
        }
        llm::Message message{role, {}};
        int i = 0;
        for (const auto& block : doc.at("content")) {
            message.content.push_back(BlockFromJson(block, "content[" + std::to_string(i++) + "]"));
        }
        return message;
    } catch (const std::runtime_error&) {
        throw;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("message decode: ") + e.what());
    }
}

}  // namespace minimal::session
