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
// the defaults for every kind a plugin does not claim). A tool RESULT row
// tries, most specific first: the ToolResultRendererRegistry by tool name,
// then the DisplayRendererRegistry by the result's display-payload kind, then
// the kind default (the disabled-plugin fallback is a fallback, not an error).

#include <chrono>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <functional>
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
#include "minimal/client/view/chat_scroll_region.hpp"

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
             const plugins::ToolResultRendererRegistry& tool_renderers,
             const plugins::DisplayRendererRegistry& display_renderers,
             std::string toggle_collapse_hint = "alt+c",
             std::string load_older_hint = "pageup");

    // `live_text_streams` is the tab's own per-segment views, index-aligned
    // with snapshot.live_segments (TabMaintenance::Maintain builds and feeds
    // them; each one's own debounce decides when it re-parses). A segment
    // index beyond the vector's size, or a Tool-kind slot, renders nothing
    // for THAT text entry -- defensive only; Maintain keeps the vector
    // sized to match every live pass.
    ftxui::Element Render(const SessionState::Snapshot& snapshot,
                          const std::vector<LiveTextStreamSlot>& live_text_streams = {});

    // Moves the scroll position by `delta_steps` half-viewport steps,
    // positive toward older messages (further back from the newest line),
    // negative toward the newest. Clamped to the content on the next
    // Render(), so this is safe to call with an out-of-range value.
    // 0 steps is a no-op; the resting position (offset 0 from the bottom)
    // reproduces the legacy pin-to-newest behavior. Scrolling is measured
    // in SCREEN LINES -- see ScrollRegion (chat_scroll_region.hpp) for why
    // the old chunk-anchor scheme was replaced.
    void ScrollBy(int delta_steps);

    // The mouse-wheel variant of ScrollBy: `delta_ticks` wheel ticks, each
    // worth kWheelLinesPerTick lines, time-gated (one step per gate window;
    // macOS trackpad momentum floods dozens of events per frame). Terminals
    // report a wheel event per scroll tick and macOS trackpad momentum
    // floods that to dozens per frame; ungated, a single flick burned
    // through the transcript faster than it could be read. Events arriving
    // within kWheelGateMs of the last APPLIED step are dropped (not
    // queued): the gate bounds the top scroll speed, and a fast flick still
    // lands because FTXUI delivers one event per frame-loop iteration, most
    // of which pass the gate at rendering pace. A tick also retires the row
    // cursor (chat.row_up/chat.row_down): the cursor's position converts to
    // a line offset first, so the wheel takes over exactly where the cursor
    // sat instead of being ignored while a cursor exists.
    void ScrollWheelBy(int delta_ticks);

    // Resets the scroll offset to the newest content -- the default state.
    // Bound to chat.scroll_bottom (PageDown) and called on every chat turn
    // submit, so sending a message while scrolled up returns to the live
    // content instead of leaving it scrolled away from what just arrived.
    void ScrollToBottom() {
        scroll_from_bottom_lines_ = 0;
        focused_row_.reset();
    }

    // True while the viewport is NOT pinned to the newest content -- scrolled
    // back, or anchored on the row cursor -- so new output lands out of view.
    // The frame says so while a turn is running.
    bool IsScrolledAway() const {
        return scroll_from_bottom_lines_ > 0 || focused_row_.has_value();
    }

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

    // Mouse-click collapse: maps a screen y (the click row) to the rendered
    // row under it and flips that row's collapse state, exactly as
    // ToggleCollapseFocused would for a focused row. Limited to the
    // COLLAPSIBLE kinds (thinking / tool activity / user attachment -- the
    // ones whose renderers draw a collapsed form): a click on a user-text or
    // markdown row returns false and the event falls through, so text
    // selection is never hijacked. The hit test runs against the boxes the
    // LAST Render() recorded (see row_boxes_); a click before the first
    // render hits nothing. True when a row was toggled.
    bool ToggleCollapseAt(int screen_y);

    // Registers the handler a click on a QUEUED (pending) message row
    // invokes: AppState::SendCancelQueuedTurn for the owning session. A
    // queued row renders a "click to cancel" affordance only when a handler
    // is set and the entry's server-assigned turn id has arrived
    // (TurnQueuedEvent); before the id, the cancel is not addressable.
    void SetCancelPendingHandler(std::function<void(std::uint64_t turn_id)> handler) {
        cancel_pending_ = std::move(handler);
    }

    // Mouse-click cancel for a QUEUED message row: hit-tests the boxes the
    // last Render() recorded for the pending rows (see pending_row_boxes_)
    // and, on a hit whose turn id is known, invokes the handler set above.
    // True when the click was consumed (the event must not fall through to
    // collapse toggling or text selection).
    bool CancelPendingAt(int screen_y);

private:
    void RebuildRows(const SessionState::Snapshot& snapshot);
    // ONE tool's live status row (the transcript's tool rows only reach a
    // page via a fetch, which mid-turn nothing submits) -- "tools as soon
    // as they are executed". Called once per Tool-kind live_segments entry,
    // interleaved with the text segments around it (track C4).
    ftxui::Element RenderLiveToolRow(const ToolExecutionState& tool) const;

    // The row cursor (focused_row_) expressed as an offset-from-bottom in
    // lines, computed against the LAST layout pass's measurements
    // (scroll_state_ + row_boxes_): the row's HEAD is revealed -- its top
    // edge pulled to the viewport top when above the viewport, to the
    // viewport bottom when below, no movement while already visible.
    // nullopt when there is nothing to compute from (cursor unset, no row
    // boxes yet, or the first frame's zero-size viewport).
    std::optional<int> RowCursorOffsetFromBottom() const;
    // Retires the row cursor into the line offset (ScrollWheelBy's first
    // duty): the wheel takes over from exactly where the cursor sat. A
    // no-op when there is no cursor or nothing to measure it against.
    void RetireRowCursorToOffset();

    using RowKey = std::pair<int64_t, int>;   // (transcript seq, occurrence within seq)

    // Three-tier collapse resolution for one row, most specific first:
    // an explicit per-row toggle > the global collapse-all choice > the
    // kind's own built-in default.
    bool IsCollapsed(const RowKey& key, const ChatRow& row) const;

    const Theme& theme_;
    const plugins::MessageRendererRegistry& message_renderers_;
    const plugins::ToolResultRendererRegistry& tool_renderers_;
    const plugins::DisplayRendererRegistry& display_renderers_;
    // The host-resolved chat.toggle_collapse binding, surfaced to row
    // renderers through RowRenderContext (they have no keymap access).
    std::string toggle_collapse_hint_ = "alt+c";
    // chat.load_older's binding, named by the "earlier messages" marker.
    std::string load_older_hint_ = "pageup";
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
    // The 4th member is RowRenderContext::continuation (the label-suppression
    // flag), for the same reason.
    std::map<std::tuple<int64_t, int, bool, bool>, ftxui::Elements> row_cache_;
    // Parallel to row_keys_: each row's kind and RESOLVED collapse state, for
    // the assembly's layout decisions (gutter color, whether consecutive
    // compact rows get a spacer) -- made outside the per-row render cache.
    std::vector<ChatRowKind> row_kinds_;
    std::vector<bool> row_collapsed_;
    // Scroll offset back from the bottom, in SCREEN LINES (see ScrollRegion
    // in chat_scroll_region.hpp). Written into scroll_state_ as the desired
    // offset every Render(); clamped there against the last measured content
    // height (a scroll issued while few rows exist, then more arrive, should
    // not leave the offset stuck out of range, and a shrunk window -- page
    // trim -- must not leave it wedged past the top). 0 pins to the newest
    // content. Ignored while focused_row_ has a value -- the row cursor
    // drives the offset instead (see Render()).
    int scroll_from_bottom_lines_ = 0;
    // Shared with the ScrollRegion node ChatView::Render wraps the column
    // in: desired offset in, applied offset + measured content/viewport
    // heights out (one frame stale, converging each layout pass).
    std::shared_ptr<ScrollState> scroll_state_ = std::make_shared<ScrollState>();
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
    // Parallel to row_keys_ (and to rendered_rows_): the rendered-position
    // box of each row (one slot per rendered part; a row whose parts
    // rendered empty holds an empty inner vector -- nothing to hit). The
    // SLOTS are persistent across assemblies (row_box_slots_ below): a
    // re-assembly reuses the same shared_ptr<Box> per (row, part) and only
    // the VALUES are overwritten by the next layout pass. That one-frame
    // staleness is load-bearing -- the row cursor's reveal offset
    // (RowCursorOffsetFromBottom) runs right after an assembly that is
    // ALWAYS stale when the cursor just moved (assembled_focused_row_ is a
    // cache key), so freshly allocated zeroed boxes would read as "row at
    // screen top, visible" and the reveal would never move. Rebuilt only
    // when the assembly cache below is stale; the box VALUES still refresh
    // every layout pass, since FTXUI's layout pass runs over whatever
    // Element tree is live that frame whether it's freshly built or reused
    // from the cache. The slots are shared with the element tree, so no
    // lifetime beyond the frame(s) that tree is current for is implied.
    std::vector<std::vector<std::shared_ptr<ftxui::Box>>> row_boxes_;
    // The persistent slot pool row_boxes_ draws from, keyed by RowKey.
    // Pruned to the current row set on every assembly (page trim / evicted
    // rows drop out).
    std::map<RowKey, std::vector<std::shared_ptr<ftxui::Box>>> row_box_slots_;

    // Assembly cache for the per-row loop in Render(): the box/spy wrapping,
    // row-cursor highlight, and timestamp formatting only need to redo their
    // work when one of the five fields below changes -- not on every FTXUI
    // repaint (mouse move, an unrelated editor keystroke).
    ftxui::Elements assembled_column_rows_;
    bool assembly_valid_ = false;
    uint64_t assembled_version_ = 0;
    std::string assembled_session_id_;
    uint64_t assembled_collapse_epoch_ = 0;
    bool assembled_show_timestamps_ = true;
    std::optional<RowKey> assembled_focused_row_;

    // Thinking segments render as plain text with no debounced reparse view
    // (unlike Text segments' MarkdownEngine::Stream) -- paragraph() re-wraps
    // the WHOLE accumulated text every Render() call otherwise, even on a
    // repaint tick with no new delta. Keyed by live_segments index; any text
    // mismatch (new turn, or genuinely new content) is simply a cache miss.
    std::vector<std::pair<std::string, ftxui::Element>> thinking_render_cache_;

    // QUEUED message rows (Snapshot::pending_user_messages): the boxes the
    // last Render() recorded, one per row (a single slot per row -- the
    // wrapper wraps row + tag), parallel to the turn ids the rows carried.
    // CancelPendingAt hit-tests these; the slot is persistent across frames
    // the way row_box_slots_ is (values refresh every layout pass).
    std::vector<std::shared_ptr<ftxui::Box>> pending_row_boxes_;
    std::vector<std::uint64_t> pending_row_turn_ids_;
    std::vector<std::shared_ptr<ftxui::Box>> pending_box_slots_;
    std::function<void(std::uint64_t turn_id)> cancel_pending_;

    // ScrollWheelBy's gate state: when the last wheel step was APPLIED. Epoch
    // (never a real timestamp) so the first event always passes.
    std::chrono::steady_clock::time_point last_wheel_step_{};
};

}  // namespace minimal::client
