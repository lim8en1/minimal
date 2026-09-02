#include "minimal/client/view/chat_view.hpp"

namespace minimal::client {

ChatView::ChatView(const Theme& theme,
                   const plugins::MessageRendererRegistry& message_renderers,
                   const plugins::ToolResultRendererRegistry& tool_renderers)
    : theme_(theme),
      message_renderers_(message_renderers),
      tool_renderers_(tool_renderers) {}

void ChatView::RebuildRows(const SessionState::Snapshot& snapshot) {
    using namespace ftxui;
    // The row cache is PER-SESSION: a transcript seq from another tab's
    // snapshot is a different message, so the cache is dropped on a session
    // switch (the version check in Render() only decides WHETHER to rebuild).
    if (rendered_session_id_ != snapshot.session_id) {
        row_cache_.clear();
    }
    const std::vector<ChatRow> rows = ChatRowsFromMessages(snapshot.scrollback_page);
    rendered_rows_.clear();
    rendered_rows_.reserve(rows.size());

    // Per-row cache keyed by (seq, occurrence within seq): a version bump
    // re-renders only rows the page did not already have. Same-key rows from
    // one message are stable (a message's row decomposition is deterministic),
    // so a cached hit is always the same visual content.
    std::map<std::pair<int64_t, int>, ftxui::Elements> fresh_cache;
    std::map<int64_t, int> occurrences;

    for (const auto& row : rows) {
        const int occurrence = occurrences[row.seq]++;
        const auto key = std::make_pair(row.seq, occurrence);

        ftxui::Elements parts;
        const auto cached = row_cache_.find(key);
        if (cached != row_cache_.end()) {
            parts = std::move(cached->second);
            fresh_cache.emplace(key, std::move(parts));
            rendered_rows_.push_back(fresh_cache.at(key));   // shared Elements, cheap copy
            continue;
        }

        // The tool-result slot: a renderer keyed by the tool's NAME takes
        // precedence on RESULT rows (it has something to render FROM). A bare
        // tool_use row (nothing produced yet) and any unmatched name fall back
        // to the message-kind default -- the "disabled tool's plugin" fallback
        // is a fallback, not an error.
        if (row.kind == ChatRowKind::ToolActivity && !row.text.empty()) {
            if (const auto* renderer = tool_renderers_.Find(row.tool_name)) {
                parts.push_back(renderer->Render(row.tool_input, row.text, row.tool_is_error,
                                                 theme_));
            }
        } else {
            const auto* renderer = message_renderers_.Find(row.kind);
            // Core fills every unclaimed kind, so null is unreachable in the
            // wired host; kept as a guard so an unwired registry degrades to an
            // empty row rather than a crash.
            if (renderer != nullptr) {
                parts.push_back(renderer->Render(row, theme_));
            }
        }
        fresh_cache.emplace(key, parts);
        rendered_rows_.push_back(std::move(parts));
    }
    row_cache_ = std::move(fresh_cache);   // evicted rows' renders drop out
    rendered_version_ = snapshot.transcript_version;
    rendered_session_id_ = snapshot.session_id;
}

ftxui::Element ChatView::Render(const SessionState::Snapshot& snapshot,
                                plugins::MarkdownEngine::Stream& streaming) {
    using namespace ftxui;

    // The cache key is (session_id, transcript_version): transcript_version is
    // a per-session counter starting at 0, so an instance re-pointed at
    // another tab whose version collided would otherwise serve the first
    // tab's cached rows as this tab's transcript. The wired host holds one
    // ChatView PER TAB (TabUi::chat), so the collision cannot happen there;
    // this check is the same invariant enforced at the unit-testable seam.
    if (rendered_version_ != snapshot.transcript_version ||
        rendered_session_id_ != snapshot.session_id) {
        RebuildRows(snapshot);
    }

    Elements column;
    column.push_back(separator());
    // The sliding-window boundary (track R2): older-page loads beyond the cap
    // evicted pages from the window's NEWEST end, so this view's top is NOT
    // the live newest message. A marker here, never a silent gap.
    if (snapshot.scrollback_trimmed) {
        column.push_back(text("··· newer messages hidden -- they return with the next turn ···") |
                         dim | color(theme_.ColorFor("system_message")));
    }
    for (auto& parts : rendered_rows_) {
        for (auto& part : parts) column.push_back(part);
        column.push_back(text(" "));  // breathing room between rows
    }

    // The in-flight block: streamed deltas via the engine's debounced view.
    // Rendered while the streaming text exists; a post-turn refresh swaps it
    // for the persisted rows (SessionState::ClearStreaming after
    // RequestScrollback's async apply).
    if (!snapshot.streaming_text.empty()) {
        column.push_back(text("assistant") | color(theme_.ColorFor("assistant_message")) | bold);
        column.push_back(streaming.Render(theme_));
    }

    // The enclosing `yframe` (app.cpp) scrolls to whatever element in this
    // subtree requests focus; with none, its offset stays 0 and it shows the
    // TOP of the transcript forever, hiding everything -- including live
    // streaming output -- past the first screen. `focus()` here is a DOM
    // marker only (Requirement::focused, not an FTXUI Component), so it
    // cannot steal keyboard focus from the prompt editor, a sibling in the
    // outer frame; it only tells THIS frame where to scroll.
    if (!column.empty()) column.back() = focus(std::move(column.back()));
    return vbox(std::move(column));
}

}  // namespace minimal::client
