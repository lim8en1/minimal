#include "minimal/client/view/chat_view.hpp"

namespace minimal::client {

namespace {

ftxui::Element DimLine(const std::string& content, const ftxui::Color& tint) {
    using namespace ftxui;
    return paragraph(content) | color(tint) | dim;
}

}  // namespace

ChatView::ChatView(const Theme& theme, const MarkdownRenderer& renderer)
    : theme_(theme), renderer_(renderer) {}

void ChatView::RebuildRows(const SessionState::Snapshot& snapshot) {
    rows_ = ChatRowsFromMessages(snapshot.scrollback_page);
    rendered_rows_.clear();
    rendered_rows_.reserve(rows_.size());
    using namespace ftxui;

    for (const auto& row : rows_) {
        Elements parts;
        switch (row.kind) {
            case ChatRowKind::UserText:
                // Verbatim: the user's text is echoed, never re-rendered as
                // markdown (their underscores are not emphasis).
                parts.push_back(paragraph(row.text) | color(theme_.ColorFor("user_message")) |
                                bold);
                break;
            case ChatRowKind::UserAttachment: {
                // Collapsed by default: the path header is the information; the
                // body is one keystroke... (v1: a dimmed preview tail).
                parts.push_back(
                    hbox({text("@") | color(theme_.ColorFor("user_message")),
                          text(row.attachment_path) | color(theme_.ColorFor("user_message")),
                          text(" (attached)") | dim}));
                parts.push_back(DimLine(row.text.substr(0, 400), theme_.ColorFor("system_message")));
                break;
            }
            case ChatRowKind::AgentMarkdown:
                parts.push_back(renderer_.Render(row.text));
                break;
            case ChatRowKind::Thinking:
                parts.push_back(hbox({text("thinking ") | color(theme_.ColorFor("thinking")) | dim,
                                      text("(" + std::to_string(static_cast<int>(row.text.size())) +
                                           " chars, hidden)") |
                                          color(theme_.ColorFor("thinking")) | dim}));
                break;
            case ChatRowKind::ToolActivity: {
                const std::string name =
                    row.tool_name.empty() ? std::string("tool") : row.tool_name;
                Elements line;
                line.push_back(text("tool " + name) | color(theme_.ColorFor("syntax_function")));
                if (row.tool_is_error) {
                    line.push_back(text(" FAILED") | color(theme_.ColorFor("error")));
                }
                parts.push_back(hbox(std::move(line)));
                if (!row.text.empty()) {
                    // Bounded body: the capped persisted ToolResultBlock is the
                    // authoritative record; this is orientation, not an archive.
                    std::string body = row.text;
                    const auto last_newline =
                        body.size() > 4000 ? body.rfind('\n', body.size() - 4000) : std::string::npos;
                    if (last_newline != std::string::npos) body = "... " + body.substr(last_newline);
                    parts.push_back(DimLine(body, theme_.ColorFor("system_message")));
                }
                break;
            }
            case ChatRowKind::System:
                // Muted and prefixed: "the harness told you something" must
                // never read as "the model said something".
                parts.push_back(
                    hbox({text("[minimal] ") | color(theme_.ColorFor("system_message")) | bold,
                          paragraph(row.text) | color(theme_.ColorFor("system_message"))}));
                break;
        }
        rendered_rows_.push_back(std::move(parts));
    }
    rendered_version_ = snapshot.transcript_version;
}

ftxui::Element ChatView::Render(const SessionState::Snapshot& snapshot,
                                StreamingMarkdownView& streaming) {
    using namespace ftxui;

    if (rendered_version_ != snapshot.transcript_version) {
        RebuildRows(snapshot);
    }

    Elements column;
    column.push_back(separator());
    for (auto& parts : rendered_rows_) {
        for (auto& part : parts) column.push_back(part);
        column.push_back(text(" "));  // breathing room between rows
    }

    // The in-flight block: streamed deltas via the debounced view. Rendered
    // while the streaming text exists; a post-turn refresh swaps it for the
    // persisted rows (SessionState::ClearStreaming after RefreshScrollback).
    if (!snapshot.streaming_text.empty()) {
        column.push_back(text("assistant") | color(theme_.ColorFor("assistant_message")) | bold);
        column.push_back(streaming.Render());
    }
    return vbox(std::move(column));
}

}  // namespace minimal::client
