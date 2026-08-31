#include "minimal/client/chat_model.hpp"

namespace minimal::client {

namespace {

std::string JoinResultText(const v1::ToolResultBlock& block) {
    std::string joined;
    for (const auto& piece : block.content()) {
        if (piece.has_text()) {
            if (!joined.empty()) joined += "\n";
            joined += piece.text().text();
        }
    }
    return joined;
}

// The spawn tool's name is core surface (agent_preset.hpp) but the wire is
// what a client sees: a ToolUseBlock named this, whose id a later
// tool-result-only User message echoes. Those messages are the harness
// delivering a subagent's completion -- not tool activity, not the model.
constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

}  // namespace

std::vector<ChatRow> ChatRowsFromMessages(const std::vector<v1::StoredMessage>& descending) {
    // Identify the spawn_subagent tool_use ids first: the completion
    // injections are distinguishable ONLY by echoing one of these.
    std::vector<std::string> spawn_ids;
    for (const auto& stored : descending) {
        for (const auto& block : stored.message().content()) {
            if (block.has_tool_use() && block.tool_use().name() == kSpawnSubagentToolName) {
                spawn_ids.push_back(block.tool_use().id());
            }
        }
    }
    const auto is_spawn_id = [&](const std::string& id) {
        for (const auto& spawn_id : spawn_ids) {
            if (id == spawn_id) return true;
        }
        return false;
    };

    // Walk ascending (reverse of the input), remembering tool names by id so a
    // result row can carry the name of the call that produced it.
    std::vector<ChatRow> rows;
    std::vector<std::pair<std::string, std::string>> tool_names;  // (id, name)
    const auto tool_name_of = [&](const std::string& id) -> std::string {
        for (const auto& [tool_id, name] : tool_names) {
            if (tool_id == id) return name;
        }
        return {};
    };

    for (auto it = descending.rbegin(); it != descending.rend(); ++it) {
        const auto& message = it->message();
        if (message.role() == v1::ROLE_ASSISTANT) {
            for (const auto& block : message.content()) {
                if (block.has_text()) {
                    rows.push_back(ChatRow{.kind = ChatRowKind::AgentMarkdown,
                                           .text = block.text().text()});
                } else if (block.has_thinking()) {
                    rows.push_back(ChatRow{.kind = ChatRowKind::Thinking,
                                           .text = block.thinking().text()});
                } else if (block.has_tool_use()) {
                    tool_names.emplace_back(block.tool_use().id(), block.tool_use().name());
                    rows.push_back(ChatRow{.kind = ChatRowKind::ToolActivity,
                                           .tool_name = block.tool_use().name()});
                }
            }
            continue;
        }

        // ROLE_USER. The harness invariant (openai_request.cpp): a User message
        // is ALL text blocks or ALL tool-result blocks, never mixed.
        bool all_results = !message.content().empty();
        for (const auto& block : message.content()) {
            if (!block.has_tool_result()) all_results = false;
        }

        if (all_results) {
            bool all_spawn = true;
            for (const auto& block : message.content()) {
                if (!is_spawn_id(block.tool_result().tool_use_id())) all_spawn = false;
            }
            if (all_spawn) {
                // One harness row per injection, text joined.
                std::string joined;
                for (const auto& block : message.content()) {
                    if (!joined.empty()) joined += "\n";
                    joined += JoinResultText(block.tool_result());
                }
                rows.push_back(ChatRow{.kind = ChatRowKind::System, .text = std::move(joined)});
                continue;
            }
            for (const auto& block : message.content()) {
                ChatRow row{.kind = ChatRowKind::ToolActivity,
                            .text = JoinResultText(block.tool_result()),
                            .tool_name = tool_name_of(block.tool_result().tool_use_id()),
                            .tool_is_error = block.tool_result().is_error()};
                rows.push_back(std::move(row));
            }
            continue;
        }

        // Text user message: block 0 is what the human typed (verbatim);
        // later text blocks are @-mention attachments (E3's expansion shape).
        for (int i = 0; i < message.content_size(); ++i) {
            const auto& block = message.content(i);
            if (!block.has_text()) continue;
            if (i == 0) {
                rows.push_back(
                    ChatRow{.kind = ChatRowKind::UserText, .text = block.text().text()});
            } else {
                ChatRow row{.kind = ChatRowKind::UserAttachment, .text = block.text().text()};
                // The E3 frame: <file path="..."> / <file-tree path="...">.
                const std::string& framed = block.text().text();
                const auto open = framed.find("<file");
                const auto path_key = framed.find("path=\"", open);
                if (path_key != std::string::npos) {
                    const auto end = framed.find('"', path_key + 6);
                    if (end != std::string::npos) {
                        row.attachment_path = framed.substr(path_key + 6, end - (path_key + 6));
                    }
                }
                rows.push_back(std::move(row));
            }
        }
    }
    return rows;
}

CommandNotice NoticeFromCommandResult(const v1::CommandResult& result) {
    CommandNotice notice;
    notice.message = result.message();
    notice.is_error = result.status() == v1::COMMAND_STATUS_ERROR;
    return notice;
}

std::optional<int64_t> NextScrollbackCursor(int64_t last_loaded_seq, bool has_earlier) {
    if (last_loaded_seq < 0) return std::nullopt;  // nothing loaded: newest page
    if (!has_earlier) return std::nullopt;         // history exhausted
    return last_loaded_seq;
}

}  // namespace minimal::client
