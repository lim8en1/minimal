#include "minimal/client/view/chat_view.hpp"

#include <algorithm>

namespace minimal::client {

ChatView::ChatView(const Theme& theme,
                   const plugins::MessageRendererRegistry& message_renderers,
                   const plugins::ToolResultRendererRegistry& tool_renderers)
    : theme_(theme),
      message_renderers_(message_renderers),
      tool_renderers_(tool_renderers) {}

bool ChatView::IsCollapsed(const RowKey& key, const ChatRow& row) const {
    if (const auto it = collapse_overrides_.find(key); it != collapse_overrides_.end()) {
        return it->second;
    }
    if (global_collapse_.has_value()) return *global_collapse_;
    return CollapsedByDefault(row);
}

void ChatView::RebuildRows(const SessionState::Snapshot& snapshot) {
    using namespace ftxui;
    // The row cache, the row cursor and any per-row collapse override are all
    // PER-SESSION: a transcript seq from another tab's snapshot is a
    // different message, so all three are dropped on a session switch (the
    // version check in Render() only decides WHETHER to rebuild).
    // global_collapse_ is NOT keyed by row, so it survives a tab switch --
    // "show me everything expanded" is a reasonable preference to carry over.
    if (rendered_session_id_ != snapshot.session_id) {
        row_cache_.clear();
        collapse_overrides_.clear();
        focused_row_.reset();
    }
    const std::vector<ChatRow> rows =
        ChatRowsFromMessages(*snapshot.scrollback_page);
    rendered_rows_.clear();
    rendered_rows_.reserve(rows.size());
    row_keys_.clear();
    row_keys_.reserve(rows.size());
    row_timestamps_.clear();
    row_timestamps_.reserve(rows.size());

    // Per-row cache keyed by (seq, occurrence within seq, resolved collapsed
    // state): a version bump re-renders only rows the page did not already
    // have; a collapse-epoch bump (no version change) re-renders only the
    // rows whose RESOLVED state actually changed -- everything else still
    // hits cache under its unchanged key. Same-key rows from one message are
    // stable (a message's row decomposition is deterministic), so a cached
    // hit is always the same visual content.
    std::map<std::tuple<int64_t, int, bool>, ftxui::Elements> fresh_cache;
    std::map<RowKey, bool> fresh_resolved;
    std::map<int64_t, int> occurrences;

    for (const auto& row : rows) {
        const int occurrence = occurrences[row.seq]++;
        const RowKey row_key = std::make_pair(row.seq, occurrence);
        row_keys_.push_back(row_key);
        row_timestamps_.push_back(row.created_at_unix_ms);
        const bool collapsed = IsCollapsed(row_key, row);
        fresh_resolved.emplace(row_key, collapsed);
        const plugins::RowRenderContext ctx{.collapsed = collapsed};
        const auto key = std::make_tuple(row.seq, occurrence, collapsed);

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
        // is a fallback, not an error. That fallback used to be reachable only
        // through the `else` branch below, which a RESULT row (non-empty text)
        // never takes -- so a completed call for any tool with no NAME-specific
        // renderer (every built-in tool: bash, edit, read, ...) rendered
        // nothing at all. The nested lookup here restores the fallback for
        // exactly that case.
        if (row.kind == ChatRowKind::ToolActivity && !row.text.empty()) {
            if (const auto* renderer = tool_renderers_.Find(row.tool_name)) {
                parts.push_back(renderer->Render(row.tool_input, row.text, row.tool_is_error,
                                                 theme_, ctx));
            } else if (const auto* fallback = message_renderers_.Find(row.kind)) {
                parts.push_back(fallback->Render(row, theme_, ctx));
            }
        } else {
            const auto* renderer = message_renderers_.Find(row.kind);
            // Core fills every unclaimed kind, so null is unreachable in the
            // wired host; kept as a guard so an unwired registry degrades to an
            // empty row rather than a crash.
            if (renderer != nullptr) {
                parts.push_back(renderer->Render(row, theme_, ctx));
            }
        }
        fresh_cache.emplace(key, parts);
        rendered_rows_.push_back(std::move(parts));
    }
    row_cache_ = std::move(fresh_cache);   // evicted rows' renders drop out
    resolved_collapsed_ = std::move(fresh_resolved);
    rendered_version_ = snapshot.transcript_version;
    rendered_session_id_ = snapshot.session_id;
    rendered_collapse_epoch_ = collapse_epoch_;
}

// One dim status row per tool call of the CURRENT turn, in gate order -- the
// same wording the running-tools panel uses ("running"/"FAILED"/"done"), so
// the two surfaces never disagree. The transcript is still authoritative:
// the post-turn page apply swaps these for the persisted tool rows.
ftxui::Elements ChatView::RenderLiveTools(const SessionState::Snapshot& snapshot) const {
    using namespace ftxui;
    Elements rows;
    for (const auto& tool : snapshot.tools) {
        Elements line;
        line.push_back(text("▎tool " + (tool.tool_name.empty() ? std::string("tool")
                                                               : tool.tool_name)) |
                       color(theme_.ColorFor("syntax_function")));
        if (tool.started) {
            line.push_back(text(" running…") | color(theme_.ColorFor("assistant_message")));
        } else if (tool.is_error) {
            line.push_back(text(" FAILED") | color(theme_.ColorFor("error")));
        } else {
            line.push_back(text(" done") | color(theme_.ColorFor("system_message")));
        }
        if (tool.summary.has_value() && !tool.summary->empty()) {
            line.push_back(text("  " + *tool.summary) | dim);
        }
        rows.push_back(hbox(std::move(line)));
    }
    return rows;
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
    // collapse_epoch_ catches a collapse toggle: it changes no
    // transcript_version, so nothing else here would notice it happened.
    if (rendered_version_ != snapshot.transcript_version ||
        rendered_session_id_ != snapshot.session_id ||
        rendered_collapse_epoch_ != collapse_epoch_) {
        RebuildRows(snapshot);
    }

    Elements column;
    // Bottom-aligns a short transcript against the enclosing yframe's
    // viewport (frame_renderer.cpp): a `flex` filler at the TOP absorbs
    // whatever vertical space the actual rows don't need. It collapses to
    // ~0 height the moment content already fills or overflows the viewport
    // (the common, longer-conversation case), so long transcripts scroll
    // exactly as before -- this only changes the "a few messages in a tall
    // terminal" case, which used to leave the conversation pinned to the top
    // with a wall of blank space beneath it, far from the input box below.
    column.push_back(filler());
    column.push_back(separator());
    // The sliding-window boundary (track R2): older-page loads beyond the cap
    // evicted pages from the window's NEWEST end, so this view's top is NOT
    // the live newest message. A marker here, never a silent gap.
    if (snapshot.scrollback_trimmed) {
        column.push_back(text("··· newer messages hidden -- they return with the next turn ···") |
                         dim | color(theme_.ColorFor("system_message")));
    }
    // Tracks which column index each ROW's first element landed at, so the
    // row cursor (focused_row_) can mark the right element for scrolling and
    // highlighting below -- a row can be several Elements wide (parts.size()
    // varies per kind), so "row index" and "column index" are not the same
    // number.
    std::vector<std::size_t> row_start_index;
    row_start_index.reserve(rendered_rows_.size());
    for (std::size_t i = 0; i < rendered_rows_.size(); ++i) {
        row_start_index.push_back(column.size());
        auto& parts = rendered_rows_[i];
        const bool is_focused = focused_row_.has_value() && i < row_keys_.size() &&
                                row_keys_[i] == *focused_row_;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            Element part = parts[p];
            // The row cursor: a left-edge marker on the row's FIRST element
            // only (a whole-row background tint would fight a code block's
            // own coloring) -- subtle, since this is a cursor to navigate
            // from, not a selection to fight visually.
            if (is_focused && p == 0) {
                part = hbox({text("▏") | color(theme_.ColorFor("row_focused")), std::move(part) | flex});
            }
            // The timestamp rides the row's FIRST element (its label line),
            // right-aligned via a flex gap -- it used to get its own row
            // below the whole thing, plus the blank line after THAT, which
            // was 3 lines of chrome per message and visually glued the
            // stamp to the NEXT speaker rather than the row it timed.
            if (p == 0 && show_timestamps_ && i < row_timestamps_.size()) {
                const auto stamp = FormatClockTime(row_timestamps_[i]);
                if (!stamp.empty()) {
                    part = hbox({std::move(part) | flex,
                                text(stamp) | dim | color(theme_.ColorFor("system_message"))});
                }
            }
            column.push_back(std::move(part));
        }
        column.push_back(text(" "));  // breathing room between rows
    }

    // The turn the user JUST submitted, echoed optimistically (state.cpp's
    // OnChatSubmitted): rendered through the same UserText renderer the
    // persisted rows use, so the echo and the row it retires look identical.
    // Positioned ahead of the streaming block -- the reply comes AFTER the
    // prompt, always. Cleared server-side of this view: a page apply past the
    // submit-time baseline swaps in the persisted (expanded) row.
    if (!snapshot.pending_user_text.empty()) {
        ChatRow echo{.kind = ChatRowKind::UserText, .text = snapshot.pending_user_text};
        if (const auto* renderer = message_renderers_.Find(ChatRowKind::UserText)) {
            column.push_back(renderer->Render(echo, theme_, plugins::RowRenderContext{}));
            column.push_back(text(" "));
        }
    }

    // The in-flight block: streamed deltas via the engine's debounced view.
    // Rendered while the streaming text exists; a post-turn refresh swaps it
    // for the persisted rows (SessionState::ClearStreaming after
    // RequestScrollback's async apply).
    if (!snapshot.streaming_text.empty()) {
        column.push_back(text("assistant") | color(theme_.ColorFor("assistant_message")) | bold);
        column.push_back(streaming.Render(theme_));
    }

    // The current turn's tool calls, live (see RenderLiveTools): a
    // tool-calling turn whose model has emitted no text yet otherwise
    // renders NOTHING until the post-turn refresh lands.
    for (auto& part : RenderLiveTools(snapshot)) {
        column.push_back(std::move(part));
    }

    // The enclosing `yframe` (app.cpp) scrolls to whatever element in this
    // subtree requests focus; with none, its offset stays 0 and it shows the
    // TOP of the transcript forever, hiding everything -- including live
    // streaming output -- past the first screen. `focus()` here is a DOM
    // marker only (Requirement::focused, not an FTXUI Component), so it
    // cannot steal keyboard focus from the prompt editor, a sibling in the
    // outer frame; it only tells THIS frame where to scroll.
    //
    // scroll_from_bottom_ picks WHICH row gets that marker: 0 (the default)
    // is column.back(), reproducing the old pin-to-newest behavior exactly.
    // Clamped here rather than in ScrollBy() -- the row count this frame is
    // the only bound that matters, and it can shrink/grow between calls.
    //
    // focused_row_ overrides that when set (chat.row_up/chat.row_down): the
    // row cursor drives scroll position instead, addressed by the START
    // index this same loop recorded above (the row can be several column
    // elements wide; the marker still only needs to land on one of them for
    // yframe to scroll the whole row into view).
    if (!column.empty()) {
        std::optional<std::size_t> focus_index;
        if (focused_row_.has_value()) {
            if (const auto it = std::find(row_keys_.begin(), row_keys_.end(), *focused_row_);
                it != row_keys_.end()) {
                const auto row_index = static_cast<std::size_t>(std::distance(row_keys_.begin(), it));
                if (row_index < row_start_index.size()) focus_index = row_start_index[row_index];
            }
        }
        if (!focus_index.has_value()) {
            scroll_from_bottom_ = std::clamp(scroll_from_bottom_, 0,
                                             static_cast<int>(column.size()) - 1);
            focus_index = column.size() - 1 - static_cast<std::size_t>(scroll_from_bottom_);
        }
        column[*focus_index] = focus(std::move(column[*focus_index]));
    }
    return vbox(std::move(column));
}

void ChatView::ScrollBy(int delta_rows) { scroll_from_bottom_ += delta_rows; }

void ChatView::MoveRowFocus(int delta) {
    if (row_keys_.empty()) return;
    long index;
    if (focused_row_.has_value()) {
        const auto it = std::find(row_keys_.begin(), row_keys_.end(), *focused_row_);
        index = (it != row_keys_.end())
                    ? static_cast<long>(std::distance(row_keys_.begin(), it))
                    : static_cast<long>(row_keys_.size()) - 1;
    } else {
        index = static_cast<long>(row_keys_.size()) - 1;   // start at the newest row
    }
    // Same sign convention as ScrollBy: positive delta moves toward OLDER
    // rows (lower index -- rows are oldest-first), negative toward newest.
    index = std::clamp<long>(index - delta, 0, static_cast<long>(row_keys_.size()) - 1);
    focused_row_ = row_keys_[static_cast<std::size_t>(index)];
}

void ChatView::ToggleCollapseFocused() {
    if (!focused_row_.has_value()) return;
    const auto it = resolved_collapsed_.find(*focused_row_);
    const bool current = it != resolved_collapsed_.end() ? it->second : false;
    collapse_overrides_[*focused_row_] = !current;
    ++collapse_epoch_;
}

void ChatView::ToggleCollapseAll() {
    global_collapse_ = !global_collapse_.value_or(false);
    collapse_overrides_.clear();
    ++collapse_epoch_;
}

}  // namespace minimal::client
