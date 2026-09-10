#include "minimal/client/view/chat_view.hpp"

#include <algorithm>

#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {

// Records where one row PART renders, in screen coordinates, so a mouse
// click can be mapped back onto the row under it (ToggleCollapseAt). FTXUI
// assigns every Node its final box during the screen's layout pass -- for a
// subtree under yframe, the Frame's SetBox has already shifted the children's
// boxes by the scroll offset, so the box a Node receives here is exactly the
// screen row it will draw at (viewport-clipped only visually). A plain Node
// subclass with a SetBox override: everything else (requirement, rendering)
// is Node's pass-through default, so the DOM is unchanged. The slot is
// shared with ChatView::row_boxes_ and overwritten on every layout pass
// (last write wins, matching what is actually drawn).
class RowBoxSpy : public ftxui::Node {
public:
    RowBoxSpy(std::shared_ptr<ftxui::Box> slot, ftxui::Element child)
        : ftxui::Node(ftxui::unpack(std::move(child))), slot_(std::move(slot)) {}

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        *slot_ = box;
        // Node::SetBox does NOT propagate to children (that is
        // NodeDecorator's behavior); without this the wrapped row is never
        // assigned its box and renders nothing.
        children_[0]->SetBox(box);
    }

private:
    std::shared_ptr<ftxui::Box> slot_;
};

// The row kinds a click may collapse: exactly those whose core renderers
// draw a distinct collapsed form (ctx.collapsed is otherwise ignored, so a
// toggle would be an invisible no-op that only hijacks the click).
bool ClickCollapsible(ChatRowKind kind) {
    switch (kind) {
        case ChatRowKind::Thinking:
        case ChatRowKind::ToolActivity:
        case ChatRowKind::UserAttachment:
            return true;
        case ChatRowKind::UserText:
        case ChatRowKind::AgentMarkdown:
        case ChatRowKind::System:
            return false;
    }
    return false;
}

}  // namespace

ChatView::ChatView(const Theme& theme,
                   const plugins::MessageRendererRegistry& message_renderers,
                   const plugins::ToolResultRendererRegistry& tool_renderers,
                   std::string toggle_collapse_hint)
    : theme_(theme),
      message_renderers_(message_renderers),
      tool_renderers_(tool_renderers),
      toggle_collapse_hint_(std::move(toggle_collapse_hint)) {}

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
        materialized_page_.reset();   // a different session's page is a different key space
    }
    // ChatRowsFromMessages is a full O(n) walk over the scrollback window (up
    // to scrollback_window_max_messages, 2000 default), copying every row's
    // text and converting every tool call's input to JSON -- Render()'s
    // rebuild gate fires on a COLLAPSE-EPOCH bump alone too (a toggle), which
    // changes no page content at all, and this used to re-walk the whole page
    // for that. scrollback_page is shared + copy-on-apply (state.hpp): its
    // POINTER IDENTITY changes only when ApplyScrollbackPage actually
    // rebuilt it, so it is exactly the right cache key -- the per-row
    // Elements cache below already handles "which RENDERS to skip"; this is
    // "skip re-deriving the ChatRows to render in the first place".
    if (materialized_page_ != snapshot.scrollback_page) {
        materialized_rows_ = ChatRowsFromMessages(*snapshot.scrollback_page);
        materialized_page_ = snapshot.scrollback_page;
    }
    const std::vector<ChatRow>& rows = materialized_rows_;
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
        // The click affordance rides the SAME ctx.collapsed resolution the
        // renderer draws from, and only for click-collapsible kinds -- a
        // plugin renderer for user text must never see a marker for a click
        // that would not do anything.
        const plugins::RowRenderContext ctx{
            .collapsed = collapsed,
            .toggle_collapse_hint = toggle_collapse_hint_,
            .collapse_marker = ClickCollapsible(row.kind) ? (collapsed ? " [+]" : " [-]")
                                                          : std::string_view("")};
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
                // Plugin code on the render path: a throw here (or in the
                // message-renderer calls below) propagated out of Render()
                // and std::terminate'd the TUI. Contained, the row degrades
                // to whatever earlier parts produced -- an empty one if none.
                try {
                    parts.push_back(renderer->Render(row.tool_input, row.text,
                                                     row.tool_is_error, theme_, ctx));
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("tool-result renderer for '{}' threw: {}",
                                                 row.tool_name, error.what());
                } catch (...) {
                    common::DefaultLogger().warn(
                        "tool-result renderer for '{}' threw a non-standard exception",
                        row.tool_name);
                }
            } else if (const auto* fallback = message_renderers_.Find(row.kind)) {
                try {
                    parts.push_back(fallback->Render(row, theme_, ctx));
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("message renderer for {} threw: {}",
                                                 ToString(row.kind), error.what());
                } catch (...) {
                    common::DefaultLogger().warn(
                        "message renderer for {} threw a non-standard exception",
                        ToString(row.kind));
                }
            }
        } else {
            const auto* renderer = message_renderers_.Find(row.kind);
            // Core fills every unclaimed kind, so null is unreachable in the
            // wired host; kept as a guard so an unwired registry degrades to an
            // empty row rather than a crash.
            if (renderer != nullptr) {
                try {
                    parts.push_back(renderer->Render(row, theme_, ctx));
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("message renderer for {} threw: {}",
                                                 ToString(row.kind), error.what());
                } catch (...) {
                    common::DefaultLogger().warn(
                        "message renderer for {} threw a non-standard exception",
                        ToString(row.kind));
                }
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

// One dim status row for ONE tool call of the CURRENT turn -- the same
// wording the running-tools panel uses ("running"/"FAILED"/"done"), so the
// two surfaces never disagree. The transcript is still authoritative: the
// post-turn page apply swaps these for the persisted tool rows.
//
// Singular (track C4): Render() calls this once per Tool-kind live_segments
// entry, in ARRIVAL order interleaved with the text segments around it,
// rather than looping the whole snapshot.tools list unconditionally last --
// see the file's Render() for why that ordering was wrong.
ftxui::Element ChatView::RenderLiveToolRow(const ToolExecutionState& tool) const {
    using namespace ftxui;
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
    return hbox(std::move(line));
}

ftxui::Element ChatView::Render(const SessionState::Snapshot& snapshot,
                                const std::vector<LiveTextStreamSlot>& live_text_streams) {
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
    // A resumed session with more history than the initial page looked like
    // its full history unless the user already knew about chat.load_older
    // (PageUp) -- has_earlier_messages was never rendered anywhere.
    if (snapshot.has_earlier_messages) {
        column.push_back(text("··· earlier messages available -- pageup to load ···") | dim |
                         color(theme_.ColorFor("system_message")));
    }
    // The sliding-window boundary (track R2): older-page loads beyond the cap
    // evicted pages from the window's NEWEST end, so this view's top is NOT
    // the live newest message. A marker here, never a silent gap.
    if (snapshot.scrollback_trimmed) {
        column.push_back(text("··· newer messages hidden -- they return with the next turn ···") |
                         dim | color(theme_.ColorFor("system_message")));
    }
    // A discontinuity INSIDE the window (older-page append landed out of
    // order -- e.g. a refresh racing a rewind): rows the transcript holds are
    // simply absent from this view, and without a marker the hole reads as
    // lost history. Same policy as the trimmed boundary: marked, never silent.
    if (snapshot.scrollback_gap) {
        column.push_back(text("··· some messages in this window could not be loaded ···") |
                         dim | color(theme_.ColorFor("system_message")));
    }
    // Tracks which column index each ROW's first element landed at, so the
    // row cursor (focused_row_) can mark the right element for scrolling and
    // highlighting below -- a row can be several Elements wide (parts.size()
    // varies per kind), so "row index" and "column index" are not the same
    // number.
    std::vector<std::size_t> row_start_index;
    row_start_index.reserve(rendered_rows_.size());
    // One box slot per row part; ToggleCollapseAt hit-tests these against a
    // click's screen y. Cleared with the frame: the spies that fill them die
    // with the previous frame's element tree.
    row_boxes_.clear();
    row_boxes_.reserve(rendered_rows_.size());
    for (std::size_t i = 0; i < rendered_rows_.size(); ++i) {
        row_start_index.push_back(column.size());
        auto& parts = rendered_rows_[i];
        const bool is_focused = focused_row_.has_value() && i < row_keys_.size() &&
                                row_keys_[i] == *focused_row_;
        std::vector<std::shared_ptr<ftxui::Box>> boxes;
        boxes.reserve(parts.size());
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
            // Outermost wrap, after the focus/timestamp decoration: the spy
            // records the box of exactly what is pushed into the column (the
            // layout-visible position), whatever the decorations did to the
            // width. Baked in at ASSEMBLY time, never into the cached
            // Elements -- a cache hit must not accumulate one spy per frame.
            auto slot = std::make_shared<ftxui::Box>();
            boxes.push_back(slot);
            column.push_back(std::make_shared<RowBoxSpy>(std::move(slot), std::move(part)));
        }
        column.push_back(text(" "));  // breathing room between rows
        row_boxes_.push_back(std::move(boxes));
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
            // A separate call from RebuildRows' own try/catch (this is the
            // live echo, not a persisted row) -- plugin code is not trusted
            // to return normally here either.
            try {
                column.push_back(renderer->Render(
                    echo, theme_,
                    plugins::RowRenderContext{.toggle_collapse_hint = toggle_collapse_hint_}));
                column.push_back(text(" "));
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("message renderer for UserText threw: {}",
                                             error.what());
            } catch (...) {
                common::DefaultLogger().warn(
                    "message renderer for UserText threw a non-standard exception");
            }
        }
    }

    // The current turn's live content, INTERLEAVED in arrival order (track
    // C4). Text and tool activity used to render as two separate groups --
    // the WHOLE streaming block first, unconditionally, then every live
    // tool row -- so a tool call in the MIDDLE of a turn (text -> tool ->
    // more text) rendered as if it happened at the very end, with the text
    // before and after it fused into one block underneath it. Each
    // live_segments entry is either a TEXT segment (fed its own debounced
    // view, live_text_streams[i]), a THINKING segment (rendered verbatim,
    // dim -- reasoning is plain text, no markdown reparse), or a TOOL
    // marker (looked up by id in
    // snapshot.tools -- RenderLiveToolRow renders whatever that tool's
    // CURRENT state is). A completed turn's page apply replaces this whole
    // section with the persisted rows above, which decompose a tool-calling
    // assistant turn into exactly this same interleaved shape
    // (ChatRowsFromMessages) -- this is what lets the live view already
    // look like where it is about to land, instead of only converging to it
    // once the turn ends.
    for (std::size_t i = 0; i < snapshot.live_segments.size(); ++i) {
        const auto& segment = snapshot.live_segments[i];
        if (segment.kind == LiveTurnSegment::Kind::Text) {
            if (i >= live_text_streams.size() || !live_text_streams[i].view) continue;
            column.push_back(text("assistant") | color(theme_.ColorFor("assistant_message")) |
                             bold);
            // A throwing stream must not collapse the whole turn's layout --
            // a short fallback line stands in so the row is still visibly
            // present, matching tab_ui.cpp's AppendDelta/Finalize containment
            // for the same plugin-owned stream object.
            try {
                column.push_back(live_text_streams[i].view->Render(theme_));
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown stream Render threw: {}", error.what());
                column.push_back(text("(render failed)") | dim);
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown stream Render threw a non-standard exception");
                column.push_back(text("(render failed)") | dim);
            }
            column.push_back(text(" "));
        } else if (segment.kind == LiveTurnSegment::Kind::Thinking) {
            // Reasoning streams as plain quiet text, same look as the persisted
            // expanded thinking row (core_registrations.cpp's
            // RenderThinking). No LiveTextStreamSlot and no markdown engine:
            // thinking renders verbatim, so a debounced reparse view would
            // only add latency -- this reads the segment text directly, the
            // same way RenderLiveToolRow reads its state.
            if (segment.text.empty()) continue;
            column.push_back(text("▎thinking") | color(theme_.ColorFor("thinking")) | dim);
            column.push_back(paragraph(segment.text) | color(theme_.ColorFor("thinking")));
            column.push_back(text(" "));
        } else if (segment.kind == LiveTurnSegment::Kind::Tool) {
            const ToolExecutionState* tool = nullptr;
            for (const auto& candidate : snapshot.tools) {
                if (candidate.tool_use_id == segment.tool_use_id) {
                    tool = &candidate;
                    break;
                }
            }
            // Not found: swept from snapshot.tools between OnTurnCompleted
            // and the post-turn refresh applying (a brief window; the
            // persisted rows are about to replace this whole section
            // anyway) -- render nothing for this marker rather than stale
            // or crashing.
            if (tool != nullptr) {
                column.push_back(RenderLiveToolRow(*tool));
                column.push_back(text(" "));
            }
        }
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
    // is column.back(), reproducing the old pin-to-newest behavior exactly
    // (including the live streaming block and live tool rows below the
    // persisted rows -- "pinned" should track whatever is newest, streaming
    // or not).
    //
    // A NONZERO value anchors on a PERSISTED row instead, counted back from
    // the newest one via row_start_index -- the same addressing
    // focused_row_ uses below, just without the visible row-cursor marker.
    // NOT column.size()-1-scroll_from_bottom_ (the pre-fix math): that
    // measured the offset from the literal end of `column`, which keeps
    // growing every delta while a turn streams (the live block) or a tool
    // runs (RenderLiveToolRow) -- so a reader scrolled up mid-turn watched
    // their own viewport slide toward the newest content on every tick,
    // even though they never touched a key. Persisted rows (row_keys_) do
    // NOT change mid-turn (the post-turn refresh is what applies a new
    // page), so anchoring on one is stable regardless of how much streams in
    // underneath. Clamped here rather than in ScrollBy() -- the row count
    // this frame is the only bound that matters, and it can shrink/grow
    // between calls.
    //
    // focused_row_ overrides both when set (chat.row_up/chat.row_down): the
    // row cursor drives scroll position instead, addressed the same way.
    if (!column.empty()) {
        std::optional<std::size_t> focus_index;
        if (focused_row_.has_value()) {
            if (const auto it = std::find(row_keys_.begin(), row_keys_.end(), *focused_row_);
                it != row_keys_.end()) {
                const auto row_index = static_cast<std::size_t>(std::distance(row_keys_.begin(), it));
                if (row_index < row_start_index.size()) focus_index = row_start_index[row_index];
            }
        }
        if (!focus_index.has_value() && !row_keys_.empty()) {
            // Clamped and STORED (not just clamped locally): a spam of
            // ScrollBy(+1) past the oldest row must not leave scroll_from_bottom_
            // ballooning past what any render can use, or "scroll back down"
            // afterward would need to burn through that same overshoot one
            // step at a time before the viewport visibly moved again.
            scroll_from_bottom_ =
                std::clamp(scroll_from_bottom_, 0, static_cast<int>(row_keys_.size()));
            if (scroll_from_bottom_ > 0) {
                const std::size_t rows_back = static_cast<std::size_t>(scroll_from_bottom_) - 1;
                const std::size_t row_index = row_keys_.size() - 1 - rows_back;
                if (row_index < row_start_index.size()) focus_index = row_start_index[row_index];
            }
        }
        if (!focus_index.has_value()) {
            scroll_from_bottom_ = 0;   // nothing to anchor on (e.g. no persisted rows yet): pin
            focus_index = column.size() - 1;
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

bool ChatView::ToggleCollapseAt(int screen_y) {
    // row_boxes_ is parallel to row_keys_ AND materialized_rows_ (RebuildRows
    // fills all three together; Render() only fills row_boxes_ when it walks
    // the same rows), so the kind gate can index materialized_rows_ directly.
    // A Render() that has not run since a session switch holds the PREVIOUS
    // session's boxes; Render()'s session check clears row_keys_ there too,
    // so sizes disagree and nothing is hit.
    for (std::size_t i = 0; i < row_boxes_.size() && i < row_keys_.size() &&
                            i < materialized_rows_.size();
         ++i) {
        const auto& boxes = row_boxes_[i];
        if (boxes.empty()) continue;   // a row that rendered nothing: nothing to hit
        // The row's full vertical extent: its first part's top to its last
        // part's bottom (parts are stacked by the column's vbox, never
        // overlapping). A click anywhere on the row -- body included --
        // toggles, not just the header line.
        int y_min = boxes.front()->y_min;
        int y_max = boxes.front()->y_max;
        for (const auto& box : boxes) {
            y_min = std::min(y_min, box->y_min);
            y_max = std::max(y_max, box->y_max);
        }
        if (screen_y < y_min || screen_y > y_max) continue;
        if (!ClickCollapsible(materialized_rows_[i].kind)) return false;
        const RowKey& key = row_keys_[i];
        const auto resolved = resolved_collapsed_.find(key);
        const bool current = resolved != resolved_collapsed_.end() ? resolved->second : false;
        collapse_overrides_[key] = !current;
        ++collapse_epoch_;
        return true;
    }
    return false;
}

}  // namespace minimal::client
