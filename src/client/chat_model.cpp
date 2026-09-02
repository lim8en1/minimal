#include "minimal/client/chat_model.hpp"

#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// google.protobuf.Struct -> JSON, for the tool_input a renderer renders FROM.
// The server has the same conversion in proto_convert.cpp; the client cannot
// link server core, and the shape is fixed by protobuf's Struct, so this is a
// twin rather than a drift risk.
nlohmann::json StructToJson(const google::protobuf::Struct& value) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, field] : value.fields()) {
        switch (field.kind_case()) {
            case google::protobuf::Value::kNullValue: out[key] = nullptr; break;
            case google::protobuf::Value::kNumberValue: out[key] = field.number_value(); break;
            case google::protobuf::Value::kStringValue: out[key] = field.string_value(); break;
            case google::protobuf::Value::kBoolValue: out[key] = field.bool_value(); break;
            case google::protobuf::Value::kListValue: {
                nlohmann::json list = nlohmann::json::array();
                for (const auto& item : field.list_value().values()) {
                    // One level of nesting is all real tool inputs use; deeper
                    // structures degrade to their string form rather than
                    // recursing here -- the authoritative record is persisted,
                    // and the renderer is a view, not a codec.
                    (void)item;
                    list.push_back(item.ShortDebugString());
                }
                out[key] = std::move(list);
                break;
            }
            case google::protobuf::Value::kStructValue:
                out[key] = StructToJson(field.struct_value());
                break;
            case google::protobuf::Value::KIND_NOT_SET: break;
        }
    }
    return out;
}

// The spawn tool's name is core surface (agent_preset.hpp) but the wire is
// what a client sees: a ToolUseBlock named this, whose id a later
// tool-result-only User message echoes. Those messages are the harness
// delivering a subagent's completion -- not tool activity, not the model.
constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

}  // namespace

std::string_view ToString(ChatRowKind kind) {
    switch (kind) {
        case ChatRowKind::UserText: return "user_text";
        case ChatRowKind::UserAttachment: return "user_attachment";
        case ChatRowKind::AgentMarkdown: return "agent_markdown";
        case ChatRowKind::Thinking: return "thinking";
        case ChatRowKind::ToolActivity: return "tool_activity";
        case ChatRowKind::System: return "system";
    }
    return "unknown";
}

std::vector<ChatRow> ChatRowsFromMessages(const std::vector<v1::StoredMessage>& descending) {
    // Identify the spawn_subagent tool_use ids first: the completion
    // injections are distinguishable ONLY by echoing one of these.
    // unordered_set, not a linear-scanned vector: a scrollback window can
    // hold up to scrollback_window_max_messages (2000 default) messages, and
    // ChatRowsFromMessages runs on every transcript_version bump -- an O(n)
    // membership test per tool-result block made the whole pass O(n^2).
    std::unordered_set<std::string> spawn_ids;
    for (const auto& stored : descending) {
        for (const auto& block : stored.message().content()) {
            if (block.has_tool_use() && block.tool_use().name() == kSpawnSubagentToolName) {
                spawn_ids.insert(block.tool_use().id());
            }
        }
    }
    const auto is_spawn_id = [&](const std::string& id) { return spawn_ids.count(id) != 0; };

    // Walk ascending (reverse of the input), remembering tool names and inputs
    // by id so a result row can carry the name of -- and the input JSON behind
    // -- the call that produced it. unordered_map for the same O(n^2)->O(n)
    // reason as spawn_ids above; emplace (not operator[]) preserves the
    // original vector scan's "first match wins" semantics for a duplicate id.
    std::vector<ChatRow> rows;
    struct ToolCall {
        std::string name;
        nlohmann::json input = nlohmann::json::object();
    };
    std::unordered_map<std::string, ToolCall> tool_calls;  // id -> call
    const auto tool_call_of = [&](const std::string& id) -> const ToolCall* {
        const auto it = tool_calls.find(id);
        return it != tool_calls.end() ? &it->second : nullptr;
    };

    for (auto it = descending.rbegin(); it != descending.rend(); ++it) {
        const auto& message = it->message();
        const int64_t message_seq = it->seq();
        const std::size_t first_row_of_message = rows.size();

        // One message's worth of row-pushing, as an IIFE rather than inline
        // loop body: every early exit below used to be a `continue` that
        // skipped the seq-stamping loop that then sat at the bottom of the
        // outer loop, so only the plain-text-user branch (the one case with
        // no early exit) ever got its rows stamped -- every assistant,
        // tool-result and harness-injection row kept seq==-1, the "synthetic"
        // sentinel. ChatView's render cache is keyed on (seq, occurrence),
        // so those rows all collapsed into one bucket keyed by POSITION
        // instead: loading an older page shifts every position, and the
        // cache then serves a stale render under the wrong message. A `return`
        // here ends the lambda, not the outer loop, so the stamp below always
        // runs regardless of which branch produced the rows.
        [&] {
            if (message.role() == v1::ROLE_ASSISTANT) {
                for (const auto& block : message.content()) {
                    if (block.has_text()) {
                        rows.push_back(ChatRow{.kind = ChatRowKind::AgentMarkdown,
                                               .text = block.text().text()});
                    } else if (block.has_thinking()) {
                        rows.push_back(ChatRow{.kind = ChatRowKind::Thinking,
                                               .text = block.thinking().text()});
                    } else if (block.has_tool_use()) {
                        // Computed ONCE, not once for the map entry and again
                        // for the row (the pre-unordered_map shape relied on
                        // vector::back() to share it; insert_or_assign here
                        // keeps that single computation without depending on
                        // "the last pushed entry" being expressible for a map).
                        nlohmann::json input = StructToJson(block.tool_use().input());
                        tool_calls.insert_or_assign(
                            block.tool_use().id(),
                            ToolCall{block.tool_use().name(), input});
                        rows.push_back(ChatRow{.kind = ChatRowKind::ToolActivity,
                                               .tool_name = block.tool_use().name(),
                                               .tool_input = std::move(input)});
                    }
                }
                return;
            }

            // ROLE_USER. The harness invariant (openai_request.cpp): a User
            // message is ALL text blocks or ALL tool-result blocks, never
            // mixed.
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
                    return;
                }
                for (const auto& block : message.content()) {
                    const ToolCall* call = tool_call_of(block.tool_result().tool_use_id());
                    ChatRow row{.kind = ChatRowKind::ToolActivity,
                                .text = JoinResultText(block.tool_result()),
                                .tool_name = call != nullptr ? call->name : std::string{},
                                .tool_input = call != nullptr ? call->input : nlohmann::json::object(),
                                .tool_is_error = block.tool_result().is_error()};
                    rows.push_back(std::move(row));
                }
                return;
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
                            row.attachment_path =
                                framed.substr(path_key + 6, end - (path_key + 6));
                        }
                    }
                    rows.push_back(std::move(row));
                }
            }
        }();

        // Stamp every row this message produced with its transcript seq --
        // the chat view's per-row render cache is keyed by it (track R3).
        // Runs unconditionally now: every branch above returns out of the
        // IIFE rather than `continue`-ing past this.
        for (auto r = rows.begin() + static_cast<std::ptrdiff_t>(first_row_of_message);
             r != rows.end(); ++r) {
            r->seq = message_seq;
        }
    }
    return rows;
}

CommandNotice NoticeFromCommandResult(const v1::CommandResult& result) {
    CommandNotice notice;
    notice.message = result.message();
    notice.is_error = result.status() == v1::COMMAND_STATUS_ERROR;
    notice.display = result.display();
    notice.view_hint = result.view_hint();
    return notice;
}

std::optional<int64_t> NextScrollbackCursor(int64_t last_loaded_seq, bool has_earlier) {
    if (last_loaded_seq < 0) return std::nullopt;  // nothing loaded: newest page
    if (!has_earlier) return std::nullopt;         // history exhausted
    return last_loaded_seq;
}

}  // namespace minimal::client
