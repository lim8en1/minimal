#include "minimal/client/view/chat_view.hpp"

namespace minimal::client {

ChatView::ChatView(const Theme& theme, const plugins::MarkdownEngine& engine,
                   const plugins::MessageRendererRegistry& message_renderers,
                   const plugins::ToolResultRendererRegistry& tool_renderers)
    : theme_(theme),
      engine_(engine),
      message_renderers_(message_renderers),
      tool_renderers_(tool_renderers) {}

void ChatView::RebuildRows(const SessionState::Snapshot& snapshot) {
    using namespace ftxui;
    rows_ = ChatRowsFromMessages(snapshot.scrollback_page);
    rendered_rows_.clear();
    rendered_rows_.reserve(rows_.size());

    for (const auto& row : rows_) {
        Elements parts;

        // The tool-result slot: a renderer keyed by the tool's NAME takes
        // precedence on RESULT rows (it has something to render FROM). A bare
        // tool_use row (nothing produced yet) and any unmatched name fall back
        // to the message-kind default -- the "disabled tool's plugin" fallback
        // is a fallback, not an error.
        if (row.kind == ChatRowKind::ToolActivity && !row.text.empty()) {
            if (const auto* renderer = tool_renderers_.Find(row.tool_name)) {
                parts.push_back(renderer->Render(row.tool_input, row.text, row.tool_is_error,
                                                 theme_));
                rendered_rows_.push_back(std::move(parts));
                continue;
            }
        }

        const auto* renderer = message_renderers_.Find(row.kind);
        // Core fills every unclaimed kind, so null is unreachable in the
        // wired host; kept as a guard so an unwired registry degrades to an
        // empty row rather than a crash.
        if (renderer != nullptr) {
            parts.push_back(renderer->Render(row, theme_));
        }
        rendered_rows_.push_back(std::move(parts));
    }
    rendered_version_ = snapshot.transcript_version;
}

ftxui::Element ChatView::Render(const SessionState::Snapshot& snapshot,
                                plugins::MarkdownEngine::Stream& streaming) {
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

    // The in-flight block: streamed deltas via the engine's debounced view.
    // Rendered while the streaming text exists; a post-turn refresh swaps it
    // for the persisted rows (SessionState::ClearStreaming after
    // RefreshScrollback).
    if (!snapshot.streaming_text.empty()) {
        column.push_back(text("assistant") | color(theme_.ColorFor("assistant_message")) | bold);
        column.push_back(streaming.Render(theme_));
    }
    return vbox(std::move(column));
}

}  // namespace minimal::client
