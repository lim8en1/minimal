#pragma once

// The chat view turns ChatRows (chat_model.hpp -- the tested data plane) into
// Elements. Row caching: markdown re-parsing every row every frame would burn
// CPU on each streamed delta, so rows are rebuilt only when the transcript
// version changes; the in-flight block goes through the engine's streaming
// view's own debounce. Scrollback is a bounded WINDOW -- a 512k-1M-token
// history is never fully materialized, which is the acceptance criterion, not
// a nicety.
//
// Per ARCHITECTURE.md "Message-kind rendering" -- presentation is now slotted
// (E5): each row renders through the MessageRendererRegistry (core supplies
// the defaults for every kind a plugin does not claim), and a tool RESULT row
// consults the ToolResultRendererRegistry by tool name FIRST, falling back to
// the default when no renderer matches (the disabled-plugin fallback is a
// fallback, not an error).

#include <ftxui/dom/elements.hpp>

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// One live TEXT segment's streaming view, owned and fed by TabMaintenance
// (the event path -- track C4), read by ChatView::Render (which must stay a
// pure read, like every other render-path consumer here). Parallel to
// SessionState::Snapshot::live_segments BY INDEX: a Tool-kind segment's slot
// stays default/unused, since only Text segments need a reparse debounce.
struct LiveTextStreamSlot {
    std::unique_ptr<plugins::MarkdownEngine::Stream> view;
    std::size_t seen = 0;             // bytes of THIS segment's text already appended
    bool finalized_for_turn = false;
};

class ChatView {
public:
    ChatView(const Theme& theme,
             const plugins::MessageRendererRegistry& message_renderers,
             const plugins::ToolResultRendererRegistry& tool_renderers);

    // `live_text_streams` is the tab's own per-segment views, index-aligned
    // with snapshot.live_segments (TabMaintenance::Maintain builds and feeds
    // them; each one's own debounce decides when it re-parses). A segment
    // index beyond the vector's size, or a Tool-kind slot, renders nothing
    // for THAT text entry -- defensive only; Maintain keeps the vector
    // sized to match every live pass.
    ftxui::Element Render(const SessionState::Snapshot& snapshot,
                          const std::vector<LiveTextStreamSlot>& live_text_streams = {});

    // Moves the scroll position by `delta_rows` rows, counted from the
    // bottom: positive scrolls toward older messages (further back from the
    // newest row), negative toward the newest. Clamped to the row count on
    // the next Render(), so this is safe to call with an out-of-range value.
    // 0 (the default) reproduces the legacy pin-to-newest behavior.
    void ScrollBy(int delta_rows);

    // Resets the scroll anchor to the newest row -- the default state.
    // Bound to chat.scroll_bottom (PageDown) and called on every chat turn
    // submit, so sending a message while scrolled up returns to the live
    // content instead of leaving it scrolled away from what just arrived.
    void ScrollToBottom() { scroll_from_bottom_ = 0; }

    // chat.toggle_timestamps: per-row clock display, layout-level (drawn by
    // this view directly, not a MessageRenderer concern -- see
    // ChatRow::created_at_unix_ms).
    void ToggleTimestamps() { show_timestamps_ = !show_timestamps_; }

    // chat.row_up/chat.row_down: moves the row cursor by `delta` rows
    // (positive = toward older, matching ScrollBy's sign convention).
    // Defaults to the newest row when nothing is focused yet. Once a row is
    // focused, IT drives scroll position instead of the bottom-pin default
    // (see Render()) -- moving the cursor is how you scroll while one is set.
    void MoveRowFocus(int delta);

    // Clears the row cursor, returning to the bottom-pin scroll default.
    // Called alongside ScrollToBottom() (chat.scroll_bottom, turn submit) and
    // on a tab switch -- a cursor left over from a DIFFERENT session's rows
    // would address nothing meaningful here.
    void ClearRowFocus() { focused_row_.reset(); }

    // chat.toggle_collapse: flips the FOCUSED row's collapse state; a no-op
    // if nothing is focused. chat.toggle_collapse_all: flips the global
    // default and clears every per-row override, so the effect is visible
    // immediately on every row at once.
    void ToggleCollapseFocused();
    void ToggleCollapseAll();

private:
    void RebuildRows(const SessionState::Snapshot& snapshot);
    // ONE tool's live status row (the transcript's tool rows only reach a
    // page via a fetch, which mid-turn nothing submits) -- "tools as soon
    // as they are executed". Called once per Tool-kind live_segments entry,
    // interleaved with the text segments around it (track C4).
    ftxui::Element RenderLiveToolRow(const ToolExecutionState& tool) const;

    using RowKey = std::pair<int64_t, int>;   // (transcript seq, occurrence within seq)

    // Three-tier collapse resolution for one row, most specific first:
    // an explicit per-row toggle > the global collapse-all choice > the
    // kind's own built-in default.
    bool IsCollapsed(const RowKey& key, const ChatRow& row) const;

    const Theme& theme_;
    const plugins::MessageRendererRegistry& message_renderers_;
    const plugins::ToolResultRendererRegistry& tool_renderers_;
    uint64_t rendered_version_ = 0;   // transcript_version these rows came from
    std::string rendered_session_id_; // which tab they came from (see Render)
    // ChatRowsFromMessages' own output, cached on scrollback_page's pointer
    // identity (see RebuildRows): a collapse-epoch-only rebuild reuses this
    // instead of re-walking the page.
    std::shared_ptr<const std::vector<v1::StoredMessage>> materialized_page_;
    std::vector<ChatRow> materialized_rows_;
    // Aligned with the rows of the current snapshot (oldest first).
    std::vector<ftxui::Elements> rendered_rows_;
    // Same order/length as rendered_rows_ -- the row cursor's addressable
    // identity list, stable across a rebuild the way a column index is not
    // (loading an older page shifts every position).
    std::vector<RowKey> row_keys_;
    // Parallel to row_keys_: each row's ChatRow::created_at_unix_ms, so
    // Render() can draw the clock (a layout concern, drawn OUTSIDE the
    // per-row render cache so toggling it needs no cache invalidation) without
    // retaining the full ChatRow past RebuildRows.
    std::vector<int64_t> row_timestamps_;
    // Per-row render cache keyed by (transcript seq, occurrence within that
    // seq, collapsed). A scrollback page bump (a version change) re-renders
    // ONLY the rows the page added -- pre-R3 every page load re-markdown-
    // rendered the whole window, including every code block. Evicted rows
    // drop out of the map. Collapsed joins the key because a toggle changes
    // what a cached render even shows -- a stale hit would serve the WRONG
    // (pre-toggle) content for that row.
    std::map<std::tuple<int64_t, int, bool>, ftxui::Elements> row_cache_;
    // Rows back from the bottom to focus on the next Render(); clamped there
    // against the current row count (a scroll issued while few rows exist,
    // then more arrive, should not leave the offset stuck out of range).
    // Ignored while focused_row_ has a value -- see Render().
    int scroll_from_bottom_ = 0;
    bool show_timestamps_ = true;

    std::optional<RowKey> focused_row_;
    std::map<RowKey, bool> collapse_overrides_;
    std::optional<bool> global_collapse_;
    // Bumped by every collapse-affecting toggle; Render() rebuilds when this
    // no longer matches rendered_collapse_epoch_ -- a toggle changes no
    // transcript_version, so nothing else would trigger a rebuild.
    uint64_t collapse_epoch_ = 0;
    uint64_t rendered_collapse_epoch_ = 0;
    // The RESOLVED collapse state each row rendered with last, keyed the same
    // way -- lets ToggleCollapseFocused flip "what's currently showing"
    // without needing the row's own ChatRow (RebuildRows is the only place
    // that has one; Elements are all that survive past it).
    std::map<RowKey, bool> resolved_collapsed_;
};

}  // namespace minimal::client
