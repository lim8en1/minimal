#include "minimal/client/view/chat_view.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>

#include "minimal/client/utf8.hpp"
#include "minimal/client/view/live_tool_row.hpp"
#include "minimal/client/view/tool_status.hpp"
#include "minimal/common/env.hpp"
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


// The live thinking render's bounds (see RenderLiveThinking): how many
// settled lines are kept/ drawn, and how many bytes of the trailing partial
// line are ever re-wrapped. The transcript is authoritative -- these cap
// presentation only (the segment-count cap's notice lives in the status
// bar's {activity} suffix; this one is an in-row "[...]").
constexpr std::size_t kMaxLiveThinkingLines = 300;
constexpr std::size_t kMaxLiveThinkingTailBytes = 64 * 1024;

// The row kinds a click may collapse: exactly those whose core renderers
// draw a distinct collapsed form (ctx.collapsed is otherwise ignored, so a
// toggle would be an invisible no-op that only hijacks the click).
bool ClickCollapsible(ChatRowKind kind) {
    switch (kind) {
        case ChatRowKind::Thinking:
        case ChatRowKind::ToolActivity:
        case ChatRowKind::UserAttachment:
        case ChatRowKind::Compaction:
            return true;
        case ChatRowKind::UserText:
        case ChatRowKind::AgentMarkdown:
        case ChatRowKind::System:
        case ChatRowKind::Switch:
            return false;
    }
    return false;
}

// Who is speaking, readable from the left edge alone while scrolling: your
// rows in your color, everything the assistant side produced (text,
// thinking, tools) in a quiet one, harness rows in theirs.
ftxui::Color GutterColor(ChatRowKind kind, const Theme& theme) {
    switch (kind) {
        case ChatRowKind::UserText:
        case ChatRowKind::UserAttachment:
            return theme.ColorFor("user_message");
        case ChatRowKind::System:
        case ChatRowKind::Compaction:
            return theme.ColorFor("system_message");
        case ChatRowKind::AgentMarkdown:
        case ChatRowKind::Thinking:
        case ChatRowKind::ToolActivity:
        case ChatRowKind::Switch:
            break;
    }
    return MutedColor(theme);
}

// A full-height speaker bar: separatorCharacter fills its whole box, so the
// bar spans every line of a multi-line row, not just its label line.
constexpr const char* kGutterGlyph = "▎";
// The row cursor's bar: a different SHAPE as well as a different color, so the
// cursor survives a colorless terminal (NO_COLOR, ColorDepth::None).
constexpr const char* kCursorGutterGlyph = "┃";

ftxui::Element WithGutter(ftxui::Element body, ftxui::Color gutter,
                          const char* glyph = kGutterGlyph) {
    using namespace ftxui;
    return hbox({separatorCharacter(glyph) | color(gutter), text(" "), std::move(body) | flex});
}

// Rows that stack without a blank line between them while collapsed: a turn
// of ten tool calls is a ten-line list, not thirty lines of chrome.
bool IsCompact(ChatRowKind kind) {
    return kind == ChatRowKind::ToolActivity || kind == ChatRowKind::Thinking;
}

// ScrollWheelBy's gate: wheel events closer together than this are dropped.
// Roughly one anchor step per two frame-loop ticks at the ~60 Hz repaint pace;
// still fast when flung, but each step is a small, readable distance.
constexpr auto kWheelGateMs = std::chrono::milliseconds(30);

// Lines one accepted wheel tick scrolls. Small enough that a multi-line
// thinking block moves past a few lines at a time (the old chunk-anchor
// scheme could leap a whole wrapped chunk -- several viewports -- per tick);
// large enough that a trackpad flick still travels. The gate bounds the
// RATE; this bounds the DISTANCE per accepted event.
constexpr int kWheelLinesPerTick = 3;

// Keyboard scroll step (chat.scroll_up/chat.scroll_down are half-viewport
// presses) before the first layout pass has measured the real viewport.
constexpr int kFallbackViewportLines = 20;

}  // namespace

ChatView::ChatView(const Theme& theme,
                   const plugins::MessageRendererRegistry& message_renderers,
                   const plugins::ToolResultRendererRegistry& tool_renderers,
                   const plugins::DisplayRendererRegistry& display_renderers,
                   std::string toggle_collapse_hint, std::string load_older_hint)
    : theme_(theme),
      message_renderers_(message_renderers),
      tool_renderers_(tool_renderers),
      display_renderers_(display_renderers),
      toggle_collapse_hint_(std::move(toggle_collapse_hint)),
      load_older_hint_(std::move(load_older_hint)) {}

bool ChatView::IsCollapsed(const RowKey& key, const ChatRow& row) const {
    if (const auto it = collapse_overrides_.find(key); it != collapse_overrides_.end()) {
        return it->second;
    }
    if (global_collapse_.has_value()) return *global_collapse_;
    // The ui.json kind defaults sit BELOW the global choice (an explicit
    // collapse-all outranks them) and ABOVE the built-in kind defaults.
    if (row.kind == ChatRowKind::Thinking && fold_thinking_default_ != kFoldDefaultAuto) {
        return fold_thinking_default_ == kFoldCollapsed;
    }
    if (row.kind == ChatRowKind::ToolActivity && fold_tool_default_ != kFoldDefaultAuto) {
        // Including rows whose display kind starts expanded (an ask's
        // recorded answers): that exception lives in the BUILT-IN tier
        // alone -- an explicit user choice ("collapsed") outranks it.
        return fold_tool_default_ == kFoldCollapsed;
    }
    // The built-in tier: a display renderer's opinion about its own kind
    // first (keyed by kind, so an aliased tool keeps it), then core's.
    if (row.kind == ChatRowKind::ToolActivity && row.tool_display.is_object()) {
        const auto kind = row.tool_display.find("kind");
        if (kind != row.tool_display.end() && kind->is_string()) {
            if (const auto* renderer = display_renderers_.Find(kind->get<std::string>())) {
                if (const auto opinion = renderer->collapsed_by_default()) return *opinion;
            }
        }
    }
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
    // A rewind/redo cuts rows whose seqs the next appends REUSE for
    // different content: everything keyed by seq is as stale as another
    // session's would be.
    if (rendered_session_id_ != snapshot.session_id ||
        rendered_rewind_epoch_ != snapshot.rewind_epoch) {
        rendered_rewind_epoch_ = snapshot.rewind_epoch;
        row_cache_.clear();
        collapse_overrides_.clear();
        focused_row_.reset();
        materialized_page_.reset();   // a different session's page is a different key space
        materialized_compactions_.reset();
        row_box_slots_.clear();       // likewise the box slots, keyed by that key space
        row_spans_.clear();           // spans are keyed by that key space too
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
    if (materialized_page_ != snapshot.scrollback_page ||
        materialized_compactions_ != snapshot.compactions) {
        materialized_rows_ =
            ChatRowsFromMessages(*snapshot.scrollback_page, snapshot.compactions.get());
        materialized_page_ = snapshot.scrollback_page;
        materialized_compactions_ = snapshot.compactions;
    }
    const std::vector<ChatRow>& rows = materialized_rows_;
    // Row windowing bookkeeping, part 1 -- captured BEFORE row_keys_ is
    // rebuilt below: row_boxes_ is a parallel VECTOR (index == position in
    // row_keys_), but its entries belong to KEYS. A page change re-derives
    // keys, and the windowed path reads boxes of rows it did not assemble
    // this frame, so each entry must follow its row across the rebuild.
    std::map<RowKey, std::vector<std::shared_ptr<ftxui::Box>>> old_row_boxes;
    for (std::size_t i = 0; i < row_keys_.size() && i < row_boxes_.size(); ++i) {
        old_row_boxes.emplace(row_keys_[i], std::move(row_boxes_[i]));
    }
    row_boxes_.clear();
    rendered_rows_.clear();
    rendered_rows_.reserve(rows.size());
    row_keys_.clear();
    row_keys_.reserve(rows.size());
    row_timestamps_.clear();
    row_timestamps_.reserve(rows.size());
    row_kinds_.clear();
    row_kinds_.reserve(rows.size());
    row_collapsed_.clear();
    row_collapsed_.reserve(rows.size());
    const std::vector<bool> continuations = LabelContinuations(rows);

    // Per-row cache keyed by (seq, content slot, resolved collapsed state):
    // a version bump re-renders only rows the page did not already have; a
    // collapse-epoch bump (no version change) re-renders only the rows whose
    // RESOLVED state actually changed -- everything else still hits cache
    // under its unchanged key. The slot names the source content block
    // (ChatRow::slot), so a key means the same content however the rest of
    // the page changes; a rewind, which reuses seqs, drops the whole cache
    // (rewind_epoch, above).
    std::map<std::tuple<int64_t, int, bool, bool>, ftxui::Elements> fresh_cache;
    std::map<RowKey, bool> fresh_resolved;

    for (const auto& row : rows) {
        const RowKey row_key = std::make_pair(row.seq, row.slot);
        row_keys_.push_back(row_key);
        row_timestamps_.push_back(row.created_at_unix_ms);
        const bool collapsed = IsCollapsed(row_key, row);
        fresh_resolved.emplace(row_key, collapsed);
        row_kinds_.push_back(row.kind);
        row_collapsed_.push_back(collapsed);
        // Joins the cache key: loading an older page can put an earlier text
        // row of the same turn in front of this one, flipping it to a
        // continuation with no change to its own content.
        const bool continuation = continuations[row_keys_.size() - 1];
        // The click affordance rides the SAME ctx.collapsed resolution the
        // renderer draws from, and only for click-collapsible kinds -- a
        // plugin renderer for user text must never see a marker for a click
        // that would not do anything.
        const plugins::RowRenderContext ctx{
            .collapsed = collapsed,
            .toggle_collapse_hint = toggle_collapse_hint_,
            .collapse_marker = ClickCollapsible(row.kind) ? (collapsed ? "▸" : "▾")
                                                          : std::string_view(""),
            .continuation = continuation};
        const auto key = std::make_tuple(row.seq, row.slot, collapsed, continuation);

        ftxui::Elements parts;
        const auto cached = row_cache_.find(key);
        if (cached != row_cache_.end()) {
            parts = std::move(cached->second);
            fresh_cache.emplace(key, std::move(parts));
            rendered_rows_.push_back(fresh_cache.at(key));   // shared Elements, cheap copy
            continue;
        }

        // A tool RESULT row, most specific renderer first: one for the tool's
        // NAME (a tool and renderer shipped as a pair), then one for the
        // result's display-payload KIND (any tool emitting it), then the
        // message-kind default. A bare tool_use row (no result yet) has no
        // text and takes the `else` branch. Renderers are plugin code on the
        // render path: a throw is contained (it used to std::terminate the
        // TUI) and the NEXT tier gets the row, so a malformed payload
        // degrades to the text row rather than to nothing.
        if (row.kind == ChatRowKind::ToolActivity && !row.text.empty()) {
            bool rendered = false;
            const auto attempt = [&](const std::string& what, const auto& render) {
                try {
                    parts.push_back(render());
                    rendered = true;
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("{} threw: {}", what, error.what());
                } catch (...) {
                    common::DefaultLogger().warn("{} threw a non-standard exception", what);
                }
            };
            const plugins::ToolResultView result{.tool_name = row.tool_name,
                                                 .tool_input = row.tool_input,
                                                 .display = row.tool_display,
                                                 .result_text = row.text,
                                                 .is_error = row.tool_is_error};
            if (const auto* renderer = tool_renderers_.Find(row.tool_name)) {
                attempt("tool-result renderer for '" + row.tool_name + "'",
                        [&] { return renderer->Render(result, theme_, ctx); });
            }
            if (!rendered && row.tool_display.is_object()) {
                const auto kind = row.tool_display.find("kind");
                if (kind != row.tool_display.end() && kind->is_string()) {
                    const auto& name = kind->get_ref<const std::string&>();
                    if (const auto* renderer = display_renderers_.Find(name)) {
                        attempt("display renderer for kind '" + name + "'",
                                [&] { return renderer->Render(result, theme_, ctx); });
                    }
                }
            }
            if (!rendered) {
                if (const auto* fallback = message_renderers_.Find(row.kind)) {
                    attempt("message renderer for " + std::string(ToString(row.kind)),
                            [&] { return fallback->Render(row, theme_, ctx); });
                }
            }
        } else {
            const auto* renderer = message_renderers_.Find(row.kind);
            // Core fills every unclaimed kind, so null is unreachable in the
            // wired host; kept as a guard so an unwired registry degrades to an
            // empty row rather than a crash.
            if (renderer != nullptr) {
                try {
                    // RenderBlocks, not Render: a long row comes back as
                    // several chunk Elements, each its own scroll anchor (the
                    // default implementation just wraps Render, so a plugin
                    // renderer that does not opt in still works, as one
                    // anchor).
                    parts = renderer->RenderBlocks(row, theme_, ctx);
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
    // Row windowing bookkeeping, part 2: refill the parallel box vector from
    // the key-remapped pool (captured above, before row_keys_ was rebuilt) --
    // rows that persisted across the rebuild keep their last measured
    // geometry; new rows start empty (never laid out). Spans for keys that
    // left the page prune with their slots.
    row_boxes_.resize(row_keys_.size());
    row_key_index_.clear();
    for (std::size_t i = 0; i < row_keys_.size(); ++i) {
        const auto it = old_row_boxes.find(row_keys_[i]);
        row_boxes_[i] = it != old_row_boxes.end()
                            ? std::move(it->second)
                            : std::vector<std::shared_ptr<ftxui::Box>>{};
        row_key_index_.emplace(row_keys_[i], i);
    }
    // A set lookup, not std::find per entry: with up to ~2000 rows in the
    // scrollback window, a linear scan per pruned entry made this O(rows^2)
    // on every page load, session switch, or collapse-all toggle.
    const std::set<RowKey> live_row_keys(row_keys_.begin(), row_keys_.end());
    for (auto it = row_spans_.begin(); it != row_spans_.end();) {
        if (live_row_keys.find(it->first) == live_row_keys.end()) {
            it = row_spans_.erase(it);
        } else {
            ++it;
        }
    }
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
ftxui::Element ChatView::RenderLiveWithPlugin(const ChatRow& row) const {
    if (!message_renderers_.PluginClaimed(row.kind)) return nullptr;
    const auto* renderer = message_renderers_.Find(row.kind);
    if (renderer == nullptr) return nullptr;
    const plugins::RowRenderContext ctx{.collapsed = false,
                                        .toggle_collapse_hint = toggle_collapse_hint_,
                                        .collapse_marker = "",
                                        .continuation = false,
                                        .live = true};
    // Plugin code on the render path, contained like the settled rows': a
    // throw falls back to core's own live rendering for this frame.
    try {
        return renderer->Render(row, theme_, ctx);
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("live {} renderer threw: {}", ToString(row.kind), error.what());
    } catch (...) {
        common::DefaultLogger().warn("live {} renderer threw a non-standard exception",
                                     ToString(row.kind));
    }
    return nullptr;
}

ftxui::Element ChatView::RenderLiveToolRow(const ToolExecutionState& tool) const {
    using namespace ftxui;
    Elements line;
    line.push_back(ToolHeadline(LiveToolStatus(tool), LiveToolName(tool), theme_));
    if (const std::string summary = LiveToolSummary(tool); !summary.empty()) {
        // The one part allowed to shrink: a long summary used to push the
        // timing field after it off the edge of the row.
        line.push_back(text("  " + summary) | Muted(theme_) | flex_shrink);
    }
    // Without a clock a stuck step and a running one looked identical until
    // the whole turn ended.
    if (const std::string timing = LiveToolTiming(tool); !timing.empty()) {
        line.push_back(text("  " + timing) | Muted(theme_));
    }
    Element row = hbox(std::move(line));
    // Persisted rows get their clock stamp from ChatRow::created_at_unix_ms
    // once scrollback catches up (see the timestamp block in Render()) --
    // but that fetch is deliberately deferred until turn-end whenever the
    // same round is also streaming text (scrollback_refresh.cpp), which left
    // a live tool call with no "when did this start" until the whole turn
    // finished. started_at_unix_ms is already on hand the instant the call
    // begins, so stamp it here too, same toggle and style as persisted rows.
    if (show_timestamps_ && tool.started_at_unix_ms.count() >= 0) {
        const auto stamp = FormatClockTime(tool.started_at_unix_ms.count());
        if (!stamp.empty()) {
            row = hbox({std::move(row) | flex,
                        text(stamp) | color(theme_.ColorFor("system_message"))});
        }
    }
    return row;
}

// Incremental plain-text wrap for ONE live thinking segment -- see the
// declaration comment. The segment's text is append-only except for its
// one-time cap truncation (TruncateUtf8 + marker REWRITES the tail), so the
// entry carries the truncated flag: a mismatch (or a size smaller than the
// rendered watermark, only truncation can shrink) forces a full rebuild,
// where the settled-lines cache starts over from byte 0.
ftxui::Element ChatView::RenderLiveThinking(const LiveTurnSegment& segment) {
    using namespace ftxui;
    const std::string& body_text = segment.Text();
    ThinkingRenderCacheEntry& entry = thinking_render_cache_[segment.segment_id];
    if (entry.rendered_bytes > body_text.size() ||
        entry.truncated != segment.text_truncated) {
        entry = ThinkingRenderCacheEntry{};
        entry.truncated = segment.text_truncated;
    }
    // Complete lines settle: wrapped once (tabs expanded, words split by
    // paragraph's own rules -- identical semantics to the old
    // paragraph(whole_text), just bounded to one line each), then reused
    // every later frame. Only the trailing partial line is re-wrapped.
    if (body_text.size() > entry.rendered_bytes) {
        if (const std::size_t last_newline = body_text.rfind('\n');
            last_newline != std::string::npos && last_newline + 1 > entry.rendered_bytes) {
            std::string_view fresh(body_text.data() + entry.rendered_bytes,
                                   last_newline + 1 - entry.rendered_bytes);
            std::size_t start = 0;
            while (start < fresh.size()) {
                const std::size_t newline = fresh.find('\n', start);
                const std::size_t end =
                    newline == std::string_view::npos ? fresh.size() : newline;
                const std::string line = ExpandTabs(fresh.substr(start, end - start));
                entry.settled_lines.push_back(paragraph(line));
                start = end + 1;
            }
            entry.rendered_bytes = last_newline + 1;
        }
        // Bound the cache's memory too, not just the drawn height: pruning
        // the front loses nothing visible while the drawn window (below)
        // still covers the kept tail.
        if (entry.settled_lines.size() > kMaxLiveThinkingLines * 2) {
            const std::size_t surplus =
                entry.settled_lines.size() - kMaxLiveThinkingLines;
            entry.settled_lines.erase(entry.settled_lines.begin(),
                                      entry.settled_lines.begin() +
                                          static_cast<std::ptrdiff_t>(surplus));
            entry.head_clipped = true;
        }
    }
    // The trailing partial line: still growing, re-wrapped per changed
    // frame -- hence the byte cap, so even a pathological no-newline stream
    // (minified JSON, base64) stays bounded.
    std::string_view tail(body_text.data() + entry.rendered_bytes,
                          body_text.size() - entry.rendered_bytes);
    bool tail_clipped = false;
    if (tail.size() > kMaxLiveThinkingTailBytes) {
        const std::size_t cut =
            Utf8CeilToBoundary(tail, tail.size() - kMaxLiveThinkingTailBytes);
        tail = tail.substr(cut);
        tail_clipped = true;
    }
    Elements body;
    if (entry.head_clipped || tail_clipped) {
        body.push_back(text("[...]"));
    }
    const std::size_t drawn =
        std::min(entry.settled_lines.size(), kMaxLiveThinkingLines);
    const auto first = entry.settled_lines.size() - drawn;
    for (std::size_t i = first; i < entry.settled_lines.size(); ++i) {
        body.push_back(entry.settled_lines[i]);   // shared Elements, cheap copy
    }
    if (!tail.empty()) {
        body.push_back(paragraph(ExpandTabs(tail)));
    }
    if (body.empty()) {
        body.push_back(text(""));
    }
    return vbox(std::move(body)) | color(theme_.ColorFor("thinking"));
}

// The kill switch (see the declaration): on unless explicitly "0", the same
// convention the ftxui overlay's MINIMAL_FTXUI_DIFF established.
bool ChatView::VirtualizationEnabled() {
    static const bool enabled = [] {
        return common::LookupEnv("MINIMAL_CHAT_VIRTUALIZATION") != "0";
    }();
    return enabled;
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
    // Before RebuildRows: the anchor is picked by key from the geometry of
    // the rows the LAST layout contained, which the rebuild re-indexes.
    AdoptAnchoredOffset();
    PickScrollAnchor();
    if (rendered_theme_epoch_ != theme_.epoch()) {
        rendered_theme_epoch_ = theme_.epoch();
        row_cache_.clear();
        thinking_render_cache_.clear();
        assembly_valid_ = false;
        ++collapse_epoch_;   // forces RebuildRows below (spans stay: heights are unchanged)
        spans_epoch_ = collapse_epoch_;
    }
    if (rendered_version_ != snapshot.transcript_version ||
        rendered_session_id_ != snapshot.session_id ||
        rendered_collapse_epoch_ != collapse_epoch_) {
        RebuildRows(snapshot);
    }

    // --- Row windowing: measurement, then selection ------------------------
    // (chat_model.hpp's SelectRowWindow; the why lives on the members and in
    // the seam's comment.) Step 1 converts the boxes the LAST layout pass
    // recorded -- screen coordinates -- into content coordinates via
    // ScrollState. ONLY the rows the last tree contained are converted: the
    // ScrollState describes that tree's layout (its applied_top), and a
    // row not in it still carries the box of an OLDER layout under a
    // different scroll offset -- but ITS span was already converted, and
    // content coordinates are immune to scroll offsets and to growth below,
    // so the stale-in-screen-terms box is fresh in content terms. Step 2
    // decides whether this frame can window at all: every row must have a
    // measured span, the viewport must have been measured, and no collapse
    // toggle may have happened since the spans were taken (heights changed
    // wholesale -- one full-assembly frame re-measures everything before
    // windowing resumes). Any unmet prerequisite runs the full-assembly
    // path below, which is also what (re-)measures, so windowing converges
    // back on its own.
    if (scroll_state_->viewport_height > 0) {
        const int viewport = scroll_state_->viewport_height;
        const int applied_top = scroll_state_->content_height - viewport -
                                scroll_state_->applied_offset_from_bottom;
        const int child_top = scroll_state_->viewport_top_screen - applied_top;
        const auto convert = [&](std::size_t i) {
            const auto& boxes = row_boxes_[i];
            if (boxes.empty()) return;   // never assembled: no geometry yet
            int y_min = boxes.front()->y_min;
            int y_max = boxes.front()->y_max;
            for (const auto& box : boxes) {
                y_min = std::min(y_min, box->y_min);
                y_max = std::max(y_max, box->y_max);
            }
            if (y_max < y_min) return;
            row_spans_[row_keys_[i]] = RowSpan{y_min - child_top, y_max - child_top};
        };
        if (last_assembly_full_) {
            for (std::size_t i = 0; i < row_keys_.size() && i < row_boxes_.size(); ++i) {
                convert(i);
            }
        } else {
            for (std::size_t i = last_window_first_;
                 i <= last_window_last_ && i < row_keys_.size() && i < row_boxes_.size();
                 ++i) {
                convert(i);
            }
        }
    }
    std::optional<RowWindowSelection> window;
    if (VirtualizationEnabled() && scroll_state_->viewport_height > 0 &&
        !row_keys_.empty() && spans_epoch_ == collapse_epoch_) {
        std::vector<RowSpan> spans;
        spans.reserve(row_keys_.size());
        bool all_known = true;
        for (const auto& key : row_keys_) {
            const auto it = row_spans_.find(key);
            if (it == row_spans_.end() || !it->second.known()) {
                all_known = false;
                break;
            }
            spans.push_back(it->second);
        }
        if (all_known) {
            const int viewport = scroll_state_->viewport_height;
            // The visible range must use the offset THIS frame will apply --
            // scroll_from_bottom_lines_, already moved by ScrollBy/wheel --
            // not ScrollState's applied_offset_from_bottom, which is the
            // PREVIOUS frame's application and would put every scroll one
            // frame late (the window chasing the viewport). It is clamped
            // against the last measured heights here; ScrollRegion's own
            // clamp (this frame's real measurements) remains the authority,
            // and the margin absorbs the difference.
            const int max_offset =
                std::max(0, scroll_state_->content_height - viewport);
            const int desired =
                std::clamp(scroll_from_bottom_lines_, 0, max_offset);
            const int sel_top = scroll_state_->content_height - viewport - desired;
            std::vector<std::size_t> force;
            const auto push_key = [&](const RowKey& key) {
                const auto it = std::find(row_keys_.begin(), row_keys_.end(), key);
                if (it != row_keys_.end()) {
                    force.push_back(static_cast<std::size_t>(it - row_keys_.begin()));
                }
            };
            if (focused_row_.has_value()) push_key(*focused_row_);
            for (const auto& key : force_include_next_) push_key(key);
            // The margin absorbs the spans' one-frame staleness: content that
            // grew below or scrolled since the spans were measured must not
            // leave rows popping in late at the viewport's edges.
            const int margin = std::clamp(viewport / 2, 10, 80);
            window = SelectRowWindow(spans, sel_top, sel_top + viewport - 1,
                                     margin, force);
        }
    }
    force_include_next_.clear();
    const bool windowed = window.has_value();
    if (!windowed) {
        // The full-assembly path re-measures every row (its layout pass runs
        // SetBox on the whole tree), so the spans recorded under THIS epoch
        // are complete afterwards and windowing can resume.
        spans_epoch_ = collapse_epoch_;
    }

    Elements column;
    const bool render_head = !windowed || window->first == 0;
    if (windowed && !render_head) {
        // Elided head: a fixed filler reproduces every line above the
        // window's first row -- top slack, boundary markers, elided rows and
        // their spacers, all measured geometry. No top flex filler: rows are
        // elided only when content already overflows the viewport. The cut
        // itself is visible on the vscroll indicator: the head/tail fillers
        // below reproduce the elided geometry, so the thumb rides the full
        // transcript's track, not the window's.
        column.push_back(filler() | size(HEIGHT, EQUAL,
                                         std::max(0, window->above_lines)));
    } else {
    // Bottom-aligns a short transcript against the enclosing yframe's
    // viewport (frame_renderer.cpp): a `flex` filler at the TOP absorbs
    // whatever vertical space the actual rows don't need. It collapses to
    // ~0 height the moment content already fills or overflows the viewport
    // (the common, longer-conversation case), so long transcripts scroll
    // exactly as before -- this only changes the "a few messages in a tall
    // terminal" case, which used to leave the conversation pinned to the top
    // with a wall of blank space beneath it, far from the input box below.
    column.push_back(filler());
    column.push_back(separator() | color(theme_.ColorFor("border_unfocused")));
    // A resumed session with more history than the initial page looked like
    // its full history unless the user already knew about chat.load_older
    // (PageUp) -- has_earlier_messages was never rendered anywhere.
    if (snapshot.has_earlier_messages) {
        column.push_back(text("··· earlier messages · " + load_older_hint_ + " to load ···") |
                         color(theme_.ColorFor("system_message")));
    }
    // The sliding-window boundary (track R2): older-page loads beyond the cap
    // evicted pages from the window's NEWEST end, so this view's top is NOT
    // the live newest message. A marker here, never a silent gap.
    if (snapshot.scrollback_trimmed) {
        column.push_back(text("··· newer messages hidden · they return with the next turn ···") |
                         color(theme_.ColorFor("system_message")));
    }
    // A discontinuity INSIDE the window (older-page append landed out of
    // order -- e.g. a refresh racing a rewind): rows the transcript holds are
    // simply absent from this view, and without a marker the hole reads as
    // lost history. Same policy as the trimmed boundary: marked, never silent.
    if (snapshot.scrollback_gap) {
        column.push_back(text("··· some messages in this window could not be loaded ···") | color(theme_.ColorFor("system_message")));
    }
    }
    // Assembly cache: the box/spy wrapping, row-cursor highlight, and
    // timestamp formatting below only need to redo their work when one of
    // these five things changes -- not on every FTXUI repaint (a mouse
    // move, an unrelated editor keystroke used to redo this whole O(rows)
    // walk, including a make_shared<Box> + make_shared<RowBoxSpy>
    // allocation and a localtime_r/snprintf call per row PART, every single
    // call). scroll_from_bottom_lines_ is deliberately NOT a key: it only
    // affects which offset ScrollRegion applies below, not any row's content.
    if (windowed) {
        // The windowed assembly: decorate ONLY the selected rows, fresh every
        // frame -- the per-row work (gutter wrap, spy reuse, timestamp) is
        // O(viewport), so the assembly cache above is unnecessary here, and
        // the window itself changes whenever the user scrolls. The
        // decoration semantics mirror the full path's loop exactly (timestamp
        // on the first part, gutter/cursor wrap per part, persistent box
        // slots, spacer rules); the two paths are kept textually separate so
        // the kill-switch path stays byte-identical to its pre-windowing
        // behavior.
        row_boxes_.resize(row_keys_.size());
        for (std::size_t i = window->first; i <= window->last && i < rendered_rows_.size(); ++i) {
            Elements parts = rendered_rows_[i];   // shared Elements, cheap copy
            const bool is_focused = focused_row_.has_value() && row_keys_[i] == *focused_row_;
            const ChatRowKind kind = row_kinds_[i];
            // PERSISTENT slots (row_box_slots_), the same pool the full path
            // draws from: a row's Box survives frames it was not assembled
            // in, so off-window rows keep their last geometry for the span
            // conversion and the hit-tests.
            std::vector<std::shared_ptr<ftxui::Box>>& slots = row_box_slots_[row_keys_[i]];
            slots.resize(parts.size());
            std::vector<std::shared_ptr<ftxui::Box>> boxes;
            boxes.reserve(slots.size());
            if (!parts.empty()) {
                // Timestamp on the row's FIRST element, right-aligned -- same
                // placement the full path uses (see its comment there).
                if (show_timestamps_ && kind != ChatRowKind::Switch &&
                    i < row_timestamps_.size()) {
                    const auto stamp = FormatClockTime(row_timestamps_[i]);
                    if (!stamp.empty()) {
                        parts[0] = hbox({std::move(parts[0]) | flex,
                                         text(stamp) | color(theme_.ColorFor("system_message"))});
                    }
                }
                std::size_t part_index = 0;
                for (auto& part : parts) {
                    if (is_focused) {
                        part = WithGutter(std::move(part),
                                          theme_.ColorForOr("cursor", "row_focused"),
                                          kCursorGutterGlyph);
                    } else if (kind != ChatRowKind::Switch) {
                        part = WithGutter(std::move(part), GutterColor(kind, theme_));
                    }
                    auto& slot = slots[part_index++];
                    if (!slot) slot = std::make_shared<ftxui::Box>();
                    boxes.push_back(slot);
                    column.push_back(std::make_shared<RowBoxSpy>(slot, std::move(part)));
                }
            }
            row_boxes_[i] = std::move(boxes);
            // Spacer between rows -- the full path's rules, with one
            // windowing exception: the window's LAST row skips its spacer
            // when rows are elided below, because SelectRowWindow's filler
            // already counts it (the elided tail's geometry includes it).
            const bool compact_run = i + 1 < row_kinds_.size() &&
                                     IsCompact(kind) && row_collapsed_[i] &&
                                     IsCompact(row_kinds_[i + 1]);
            const bool spacer_skipped =
                (i == window->last && window->rows_elided_below) || compact_run;
            if (!spacer_skipped) column.push_back(text(" "));
        }
        if (window->rows_elided_below) {
            column.push_back(filler() | size(HEIGHT, EQUAL, window->below_lines));
        }
        last_assembly_full_ = false;
        last_window_first_ = window->first;
        last_window_last_ = window->last;
    } else {
    const bool assembly_stale =
        !assembly_valid_ || assembled_version_ != rendered_version_ ||
        assembled_session_id_ != rendered_session_id_ ||
        assembled_collapse_epoch_ != rendered_collapse_epoch_ ||
        assembled_show_timestamps_ != show_timestamps_ ||
        assembled_focused_row_ != focused_row_;
    if (assembly_stale) {
        assembled_column_rows_.clear();
        assembled_column_rows_.reserve(rendered_rows_.size() * 2);
        // One box slot per row; ToggleCollapseAt hit-tests these against a
        // click's screen y, and the row cursor's reveal-offset math reads
        // them too (see RowCursorOffsetFromBottom).
        row_boxes_.clear();
        row_boxes_.reserve(rendered_rows_.size());
        for (std::size_t i = 0; i < rendered_rows_.size(); ++i) {
            Elements parts = rendered_rows_[i];   // shared Elements, cheap copy
            const bool is_focused = focused_row_.has_value() && i < row_keys_.size() &&
                                    row_keys_[i] == *focused_row_;
            const ChatRowKind kind =
                i < row_kinds_.size() ? row_kinds_[i] : ChatRowKind::AgentMarkdown;
            // PERSISTENT slots (row_box_slots_): re-assembly reuses the same
            // Box per (row, part) so the values from the PREVIOUS layout pass
            // survive until the next one overwrites them. The reveal-offset
            // math runs right after an assembly that is always stale when the
            // cursor just moved -- fresh zeroed boxes there would read as
            // "row at screen top, already visible" and the reveal would
            // never move.
            std::vector<std::shared_ptr<ftxui::Box>>& slots =
                row_box_slots_[i < row_keys_.size() ? row_keys_[i] : RowKey{}];
            slots.resize(parts.size());
            std::vector<std::shared_ptr<ftxui::Box>> boxes;
            boxes.reserve(slots.size());
            if (!parts.empty()) {
                // The timestamp rides the row's FIRST element (its label line),
                // right-aligned via a flex gap -- it used to get its own row
                // below the whole thing, plus the blank line after THAT, which
                // was 3 lines of chrome per message and visually glued the
                // stamp to the NEXT speaker rather than the row it timed.
                // Not on a Switch divider: it is a rule, and the stamp would
                // only repeat the reply's own right below it.
                if (show_timestamps_ && kind != ChatRowKind::Switch &&
                    i < row_timestamps_.size()) {
                    const auto stamp = FormatClockTime(row_timestamps_[i]);
                    if (!stamp.empty()) {
                        parts[0] = hbox({std::move(parts[0]) | flex,
                                         text(stamp) | color(theme_.ColorFor("system_message"))});
                    }
                }
                // ONE spy per PART, not one per row: a row's rendered extent
                // is the union of its parts' boxes, and multi-part rows
                // (chunked RenderBlocks output) need every part's box for
                // the click hit-test and the row-cursor reveal offset. The
                // gutter wraps every part so the speaker bar stays
                // continuous across the row's full height
                // (separatorCharacter fills each part's box); the timestamp
                // rode the first part above.
                std::size_t part_index = 0;
                for (auto& part : parts) {
                    if (is_focused) {
                        part = WithGutter(std::move(part),
                                          theme_.ColorForOr("cursor", "row_focused"),
                                          kCursorGutterGlyph);
                    } else if (kind != ChatRowKind::Switch) {
                        part = WithGutter(std::move(part), GutterColor(kind, theme_));
                    }
                    // Outermost wrap, after the gutter/timestamp decoration: the
                    // spy records the box of exactly what is pushed into the
                    // column (the layout-visible position). Baked in at ASSEMBLY
                    // time, never into the cached Elements -- a cache hit must
                    // not accumulate one spy per frame. The slot itself is
                    // REUSED from row_box_slots_ (allocated once, here, when a
                    // row or part first appears).
                    auto& slot = slots[part_index++];
                    if (!slot) slot = std::make_shared<ftxui::Box>();
                    boxes.push_back(slot);
                    assembled_column_rows_.push_back(
                        std::make_shared<RowBoxSpy>(slot, std::move(part)));
                }
            }
            // Breathing room between rows -- except inside a run of collapsed
            // tool/thinking rows, which reads as one compact list. The LAST
            // row always gets it: the pin-to-newest default (offset 0 from
            // the bottom) keeps the viewport's bottom line on this one-line
            // spacer, never mid-way through a tall row's body.
            const bool compact_run = i + 1 < row_kinds_.size() && i < row_collapsed_.size() &&
                                     IsCompact(kind) && row_collapsed_[i] &&
                                     IsCompact(row_kinds_[i + 1]);
            if (!compact_run) assembled_column_rows_.push_back(text(" "));
            row_boxes_.push_back(std::move(boxes));
        }
        // Prune slots for rows no longer in the window (page trim, eviction,
        // session switch): the pool must not outgrow the rendered rows. A
        // set lookup, not std::find per entry -- see RebuildRows's identical
        // fix for row_spans_.
        const std::set<RowKey> live_row_keys(row_keys_.begin(), row_keys_.end());
        for (auto it = row_box_slots_.begin(); it != row_box_slots_.end();) {
            if (live_row_keys.find(it->first) == live_row_keys.end()) {
                it = row_box_slots_.erase(it);
            } else {
                ++it;
            }
        }
        assembled_version_ = rendered_version_;
        assembled_session_id_ = rendered_session_id_;
        assembled_collapse_epoch_ = rendered_collapse_epoch_;
        assembled_show_timestamps_ = show_timestamps_;
        assembled_focused_row_ = focused_row_;
        assembly_valid_ = true;
        last_assembly_full_ = true;
    }
    column.insert(column.end(), assembled_column_rows_.begin(), assembled_column_rows_.end());
    }

    // The turns the user submitted that have not landed as persisted rows
    // yet (state.cpp's OnChatSubmitted FIFO): rendered through the same
    // UserText renderer the persisted rows use, one row each, in submit
    // order, ahead of the streaming block -- the replies come AFTER the
    // prompts, always. A mid-turn submit is QUEUED server-side, so each row
    // carries a queued tag; once the entry's server-assigned turn id has
    // arrived (TurnQueuedEvent) the row is addressable, and a click cancels
    // the queued message before it ever runs. Retired server-side of this
    // view: a page apply pairs each entry's persisted row with it (FIFO).
    pending_row_boxes_.clear();
    pending_row_turn_ids_.clear();
    if (pending_box_slots_.size() < snapshot.pending_user_messages.size()) {
        pending_box_slots_.resize(snapshot.pending_user_messages.size());
    }
    if (const auto* renderer = message_renderers_.Find(ChatRowKind::UserText)) {
        for (std::size_t i = 0; i < snapshot.pending_user_messages.size(); ++i) {
            const auto& pending = snapshot.pending_user_messages[i];
            ChatRow echo{.kind = ChatRowKind::UserText, .text = pending.text};
            // A separate call from RebuildRows' own try/catch (this is the
            // live echo, not a persisted row) -- plugin code is not trusted
            // to return normally here either.
            try {
                Elements parts;
                parts.push_back(
                    renderer->Render(
                        echo, theme_,
                        plugins::RowRenderContext{.toggle_collapse_hint = toggle_collapse_hint_}));
                // The cancel affordance rides the row, not a second line:
                // one line of chrome, and the hit-test wraps the whole thing
                // either way. Before the turn id arrives the cancel is not
                // addressable -- say queued, not clickable.
                const bool cancellable = cancel_pending_ != nullptr && pending.turn_id != 0;
                parts.push_back(text(cancellable ? "  queued · click to cancel"
                                                 : "  queued") |
                                color(theme_.ColorFor("system_message")));
                // ONE persistent box slot per pending row: the spy records the
                // row's rendered position every layout pass (last write wins)
                // for CancelPendingAt's hit test, the same way row_boxes_ works.
                auto& slot = pending_box_slots_[i];
                if (!slot) slot = std::make_shared<ftxui::Box>();
                pending_row_boxes_.push_back(slot);
                pending_row_turn_ids_.push_back(pending.turn_id);
                column.push_back(ftxui::Element(std::make_shared<RowBoxSpy>(
                    slot, WithGutter(vbox(std::move(parts)),
                                     GutterColor(ChatRowKind::UserText, theme_)))));
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
    // view, looked up by the segment's stable id in live_text_streams), a
    // THINKING segment (rendered verbatim,
    // dim -- reasoning is plain text, no markdown reparse), or a TOOL
    // marker (looked up by id in
    // snapshot.tools -- RenderLiveToolRow renders whatever that tool's
    // CURRENT state is). A completed turn's page apply replaces this whole
    // section with the persisted rows above, which decompose a tool-calling
    // assistant turn into exactly this same interleaved shape
    // (ChatRowsFromMessages) -- this is what lets the live view already
    // look like where it is about to land, instead of only converging to it
    // once the turn ends.
    const ftxui::Color assistant_gutter = GutterColor(ChatRowKind::AgentMarkdown, theme_);
    bool live_labeled = false;   // the live turn's label, once -- LabelContinuations' rule
    // Prune the thinking cache to the ids actually live (removals are
    // permanent within a turn -- see LiveTurnSegment::segment_id), so a
    // trimmed/retired segment's wrapped lines cannot pile up unbounded.
    {
        std::vector<uint64_t> live_thinking_ids;
        live_thinking_ids.reserve(snapshot.live_segments.size());
        for (const auto& segment : snapshot.live_segments) {
            if (segment.kind == LiveTurnSegment::Kind::Thinking) {
                live_thinking_ids.push_back(segment.segment_id);
            }
        }
        for (auto it = thinking_render_cache_.begin();
             it != thinking_render_cache_.end();) {
            if (std::find(live_thinking_ids.begin(), live_thinking_ids.end(),
                          it->first) == live_thinking_ids.end()) {
                it = thinking_render_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Each live segment's outermost wrap is a spy on its persistent slot
    // (live_box_slots_), the scroll anchor's candidates inside the live turn.
    std::vector<std::shared_ptr<ftxui::Box>> live_boxes;
    const auto spy_live = [&](uint64_t segment_id, Element element) {
        auto& slot = live_box_slots_[segment_id];
        if (!slot) slot = std::make_shared<ftxui::Box>();
        live_boxes.push_back(slot);
        return Element(std::make_shared<RowBoxSpy>(slot, std::move(element)));
    };
    for (auto it = live_box_slots_.begin(); it != live_box_slots_.end();) {
        const bool live = std::any_of(
            snapshot.live_segments.begin(), snapshot.live_segments.end(),
            [&](const LiveTurnSegment& segment) { return segment.segment_id == it->first; });
        it = live ? std::next(it) : live_box_slots_.erase(it);
    }
    for (std::size_t i = 0; i < snapshot.live_segments.size(); ++i) {
        const auto& segment = snapshot.live_segments[i];
        if (segment.kind == LiveTurnSegment::Kind::Text) {
            // The slot is found by the segment's stable id, not this loop's
            // index: mid-turn removals (the page apply's retirement, the
            // count-cap trim) shift indices -- see TabMaintenance::Maintain.
            const LiveTextStreamSlot* slot = nullptr;
            for (const auto& candidate : live_text_streams) {
                if (candidate.segment_id == segment.segment_id && candidate.view) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot == nullptr) continue;
            Elements block;
            if (!live_labeled) {
                // The same label the settled row will carry. The EFFECTIVE
                // agent/model is what this turn runs on (a staged /model
                // applies at the next turn boundary); desired only fills in on
                // a tab whose metadata has not arrived yet. The post-turn page
                // apply swaps in the persisted attribution either way.
                const std::string& live_agent = !snapshot.agent_preset.empty()
                                                    ? snapshot.agent_preset
                                                    : snapshot.desired_agent_preset;
                const std::string& live_model =
                    !snapshot.model.empty() ? snapshot.model : snapshot.desired_model;
                block.push_back(text(AssistantLabel(live_agent, live_model)) |
                                color(theme_.ColorFor("assistant_message")) | bold);
                live_labeled = true;
            }
            // A throwing stream must not collapse the whole turn's layout --
            // a short fallback line stands in so the row is still visibly
            // present, matching tab_ui.cpp's AppendDelta/Finalize containment
            // for the same plugin-owned stream object.
            try {
                // Base body color, as on the settled row (core_registrations.cpp).
                block.push_back(slot->view->Render(theme_) |
                                color(theme_.ColorFor("assistant_message")));
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown stream Render threw: {}", error.what());
                block.push_back(text("(render failed)") | Muted(theme_));
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown stream Render threw a non-standard exception");
                block.push_back(text("(render failed)") | Muted(theme_));
            }
            column.push_back(spy_live(segment.segment_id,
                                      WithGutter(vbox(std::move(block)), assistant_gutter)));
            column.push_back(text(" "));
        } else if (segment.kind == LiveTurnSegment::Kind::Thinking) {
            // Reasoning streams as plain quiet text, same look as the persisted
            // expanded thinking row (core_registrations.cpp's
            // RenderThinking). No LiveTextStreamSlot and no markdown engine:
            // thinking renders verbatim, so a debounced reparse view would
            // only add latency -- but the WRAPPING is incremental
            // (RenderLiveThinking): the old paragraph(whole_text) per
            // changed frame re-split the entire accumulated block into word
            // Elements up to 20x/s, which froze the frame loop on a long
            // reasoning stream.
            if (segment.Text().empty()) continue;
            ChatRow live_row{.kind = ChatRowKind::Thinking, .text = segment.Text()};
            if (Element plugin_row = RenderLiveWithPlugin(live_row)) {
                column.push_back(spy_live(segment.segment_id,
                                          WithGutter(std::move(plugin_row), assistant_gutter)));
                column.push_back(text(" "));
                continue;
            }
            ftxui::Element thinking_body = RenderLiveThinking(segment);
            column.push_back(spy_live(
                segment.segment_id,
                WithGutter(
                    vbox({text("thinking") | color(theme_.ColorFor("thinking")), thinking_body}),
                    assistant_gutter)));
            column.push_back(text(" "));
        } else if (segment.kind == LiveTurnSegment::Kind::Tool) {
            const ToolExecutionState* tool = nullptr;
            for (const auto& candidate : snapshot.tools) {
                if (candidate.tool_use_id == segment.tool_use_id) {
                    tool = &candidate;
                    break;
                }
            }
            // Not found: OnTurnCompleted swept the row, and the marker is
            // waiting for the post-turn refresh to retire it. It carries the
            // row's final state for exactly this window -- rendering nothing
            // here is how a late refresh made the turn's tool calls vanish.
            if (tool == nullptr) tool = segment.final_tool.get();
            if (tool != nullptr) {
                ChatRow live_row{.kind = ChatRowKind::ToolActivity,
                                 .tool_name = tool->tool_name,
                                 .tool_input = *tool->input,
                                 .tool_is_error = tool->is_error,
                                 .tool_pending = tool->started};
                Element plugin_row = RenderLiveWithPlugin(live_row);
                column.push_back(spy_live(segment.segment_id,
                                          WithGutter(plugin_row ? std::move(plugin_row)
                                                                : RenderLiveToolRow(*tool),
                                                     assistant_gutter)));
                // Consecutive calls stack as a list, like the settled rows.
                const bool next_is_tool = i + 1 < snapshot.live_segments.size() &&
                                          snapshot.live_segments[i + 1].kind ==
                                              LiveTurnSegment::Kind::Tool;
                if (!next_is_tool) column.push_back(text(" "));
            }
        }
    }

    // Scroll position: ScrollRegion (the wrapper this returns) applies an
    // explicit offset-from-bottom in lines -- no focus() marker anywhere in
    // the column anymore. 0 (the default) pins to the newest content,
    // including the live streaming block and live tool rows below the
    // persisted rows ("pinned" tracks whatever is newest, streaming or not).
    // Scrolled away, the region additionally ANCHORS the row at the
    // viewport's top (PickScrollAnchor / ScrollState::anchor): an offset
    // from the bottom alone moved a reader parked mid-transcript by every
    // line that streamed in below.
    //
    // The row cursor (focused_row_, chat.row_up/chat.row_down) overrides the
    // stored offset: the focused row is revealed minimally (see
    // RowCursorOffsetFromBottom). The stored value is clamped here rather
    // than in ScrollBy() -- the content height this frame measures is the
    // only bound that matters, and it can shrink (page trim) or grow
    // (streaming) between calls. Clamping is STORED, not just local: a spam
    // of scroll-ups past the top must not leave the offset ballooning past
    // what any render can use, or scrolling back down would burn through
    // that same overshoot before the viewport visibly moved again.
    if (focused_row_.has_value()) {
        if (const auto offset = RowCursorOffsetFromBottom()) {
            scroll_from_bottom_lines_ = *offset;
        }
    } else if (scroll_state_->viewport_height > 0) {
        // First frame: nothing measured yet -- pass the offset through
        // unclamped and let ScrollRegion's own clamp (which sees the REAL
        // content height) bound it; the stored value catches up next pass.
        const int max_from_bottom = std::max(
            0, scroll_state_->content_height - scroll_state_->viewport_height);
        scroll_from_bottom_lines_ =
            std::clamp(scroll_from_bottom_lines_, 0, max_from_bottom);
    }
    scroll_state_->desired_offset_from_bottom = scroll_from_bottom_lines_;
    {
        const std::size_t live_tail =
            snapshot.live_segments.empty() ? 0 : snapshot.live_segments.back().Text().size();
        const auto signature = std::make_tuple(
            row_keys_.empty() ? RowKey{} : row_keys_.back(), snapshot.live_segments.size(),
            live_tail, snapshot.pending_user_messages.size());
        if (signature != bottom_signature_ && IsScrolledAway()) unseen_below_ = true;
        bottom_signature_ = signature;
    }
    live_boxes_ = std::move(live_boxes);
    // Hand the anchor's persistent box to the region (null: pinned,
    // cursor-driven, or just scrolled; a box whose row/segment left the
    // transcript is not laid out, and the region ignores it).
    scroll_state_->anchor = anchor_box_;
    scroll_state_->anchor_line = anchor_line_;
    return ScrollRegion(vscroll_indicator(vbox(std::move(column))), scroll_state_);
}

void ChatView::PickScrollAnchor() {
    anchor_box_.reset();
    if (scroll_from_bottom_lines_ <= 0 && !focused_row_.has_value()) unseen_below_ = false;
    const bool suspended = anchor_suspended_;
    anchor_suspended_ = false;
    if (suspended || focused_row_.has_value() || scroll_from_bottom_lines_ <= 0 ||
        scroll_state_->viewport_height <= 0) {
        return;
    }
    // Only rows the last tree contained carry that layout's geometry.
    std::size_t first = 0;
    std::size_t last = row_keys_.empty() ? 0 : row_keys_.size() - 1;
    if (!last_assembly_full_) {
        first = last_window_first_;
        last = last_window_last_;
    }
    const int top = scroll_state_->viewport_top_screen;
    std::optional<int> best_below;   // nearest part starting below the top line
    // True once the part containing the top line is found.
    const auto consider = [&](const std::shared_ptr<ftxui::Box>& box) {
        if (box == nullptr || box->y_max < box->y_min) return false;
        if (box->y_min <= top && top <= box->y_max) {
            anchor_box_ = box;
            anchor_line_ = top - box->y_min;
            return true;
        }
        if (box->y_min > top && (!best_below || box->y_min < *best_below)) {
            best_below = box->y_min;
            anchor_box_ = box;
            anchor_line_ = top - box->y_min;   // negative: the top sits on a gap
        }
        return false;
    };
    for (std::size_t i = first; i <= last && i < row_keys_.size() && i < row_boxes_.size(); ++i) {
        for (const auto& box : row_boxes_[i]) {
            if (consider(box)) return;
        }
    }
    // The live turn renders below every row: a reader parked inside the
    // streaming reply anchors on its segment.
    for (const auto& box : live_boxes_) {
        if (consider(box)) return;
    }
}

void ChatView::ScrollBy(int delta_steps) {
    AdoptAnchoredOffset();
    anchor_suspended_ = true;
    RetireRowCursorToOffset();
    const int viewport = scroll_state_->viewport_height > 0
                             ? scroll_state_->viewport_height
                             : kFallbackViewportLines;
    scroll_from_bottom_lines_ += delta_steps * std::max(3, viewport / 2);
}

void ChatView::ScrollWheelBy(int delta_ticks) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_wheel_step_ < kWheelGateMs) return;   // momentum burst: dropped
    last_wheel_step_ = now;
    AdoptAnchoredOffset();
    anchor_suspended_ = true;
    RetireRowCursorToOffset();
    scroll_from_bottom_lines_ += delta_ticks * kWheelLinesPerTick;
}

std::optional<int> ChatView::RowCursorOffsetFromBottom() const {
    if (!focused_row_.has_value() || scroll_state_->viewport_height <= 0) {
        return std::nullopt;
    }
    const auto it = row_key_index_.find(*focused_row_);
    if (it == row_key_index_.end()) return std::nullopt;
    const std::size_t row_index = it->second;
    if (row_index >= row_boxes_.size() || row_boxes_[row_index].empty()) {
        return std::nullopt;
    }
    // The row's full vertical extent, from the boxes the last layout pass
    // recorded (screen coordinates -- RowBoxSpy runs under the scroll
    // offset, exactly like the click hit-test's assumption).
    int y_min = row_boxes_[row_index].front()->y_min;
    int y_max = row_boxes_[row_index].front()->y_max;
    for (const auto& box : row_boxes_[row_index]) {
        y_min = std::min(y_min, box->y_min);
        y_max = std::max(y_max, box->y_max);
    }
    // Screen -> viewport-relative coordinates: the row's edges as LAST laid
    // out, measured against the viewport's own top line. (Content
    // coordinates would need top_applied folded in; viewport-relative is
    // the same thing without the arithmetic, and the reveal thresholds
    // below are viewport-relative by nature -- comparing an absolute
    // content coordinate against the viewport HEIGHT instead always reads
    // "below the fold" once content exceeds one screen, slamming the cursor
    // reveal to the bottom.)
    const int viewport = scroll_state_->viewport_height;
    const int top_applied = scroll_state_->content_height - viewport -
                            scroll_state_->applied_offset_from_bottom;
    const int row_top = y_min - scroll_state_->viewport_top_screen;
    // Reveal the row's HEAD (its top edge -- the label line a chat row is
    // recognized by): scroll until the top edge is on screen, at the
    // viewport's top when the row sits above, at its bottom when below; no
    // movement while the head is already visible. Minimal per move, and
    // IDEMPOTENT -- re-running it from the revealed position moves nothing,
    // which matters because the reveal is recomputed every frame while the
    // cursor sits on a row (a bottom-edge reveal of a tall row would
    // otherwise drag the view to the row's tail on the frame after it
    // showed the head). Centering (yframe's old behavior) is deliberately
    // avoided -- it leapt past readable content whenever the target was
    // taller than the viewport.
    int top = top_applied;
    if (row_top < 0) {
        top = top_applied + row_top;
    } else if (row_top >= viewport) {
        top = top_applied + (row_top - viewport + 1);
    }
    const int max_top = std::max(0, scroll_state_->content_height - viewport);
    top = std::clamp(top, 0, max_top);
    return max_top - top;
}

void ChatView::RetireRowCursorToOffset() {
    if (!focused_row_.has_value()) return;
    if (const auto offset = RowCursorOffsetFromBottom()) {
        scroll_from_bottom_lines_ = *offset;
    }
    focused_row_.reset();
}

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
    // The toggled row's height just changed; keep it rendered while the
    // spans re-measure (the epoch mismatch already forces one full frame,
    // but a same-frame selection must not elide the row the user clicked).
    force_include_next_.insert(*focused_row_);
}

void ChatView::ToggleCollapseAll() {
    global_collapse_ = !global_collapse_.value_or(false);
    collapse_overrides_.clear();
    ++collapse_epoch_;
}

namespace {
// Shared body of ToggleThinkingFold/ToggleToolFold: the kind's default flips
// to the OPPOSITE concrete state of what the kind currently SHOWS as -- the
// global collapse choice when one is set (otherwise the toggle would be
// invisible under it), else the kind's default tier: the ui.json value when
// set, else the built-in kind default (Thinking collapsed; ToolActivity
// whatever CollapsedByDefault says). Per-row overrides are not consulted --
// they vary per row and are cleared right after anyway. kFoldDefaultAuto is
// never assigned in-session: "auto" is a ui.json-only reading, restored at
// the next launch.
bool KindShowsCollapsed(const std::optional<bool>& global_collapse, int kind_default,
                        bool builtin_collapsed) {
    if (global_collapse.has_value()) return *global_collapse;
    if (kind_default != kFoldDefaultAuto) return kind_default == kFoldCollapsed;
    return builtin_collapsed;
}
}  // namespace

int ChatView::ToggleThinkingFold() {
    const int next = KindShowsCollapsed(global_collapse_, fold_thinking_default_,
                                        /*builtin_collapsed=*/true)
                         ? kFoldExpanded
                         : kFoldCollapsed;
    fold_thinking_default_ = next;
    collapse_overrides_.clear();
    global_collapse_.reset();
    ++collapse_epoch_;
    return next;
}

int ChatView::ToggleToolFold() {
    const int next =
        KindShowsCollapsed(
            global_collapse_, fold_tool_default_,
            CollapsedByDefault(ChatRow{.kind = ChatRowKind::ToolActivity}))
            ? kFoldExpanded
            : kFoldCollapsed;
    fold_tool_default_ = next;
    collapse_overrides_.clear();
    global_collapse_.reset();
    ++collapse_epoch_;
    return next;
}

bool ChatView::ToggleCollapseAt(int screen_y) {
    // Off-viewport rows keep their last boxes (see ViewportContainsY): a
    // click on the editor or a panel must not toggle a row scrolled out of
    // view behind it.
    if (!ViewportContainsY(screen_y)) return false;
    // row_boxes_ is parallel to row_keys_ AND materialized_rows_ (RebuildRows
    // fills all three together; Render() only fills row_boxes_ when it walks
    // the same rows), so the kind gate can index materialized_rows_ directly.
    // A Render() that has not run since a session switch holds the PREVIOUS
    // session's boxes; Render()'s session check clears row_keys_ there too,
    // so sizes disagree and nothing is hit.
    for (std::size_t i = 0; i < row_boxes_.size() && i < row_keys_.size() &&
                            i < materialized_rows_.size();
         ++i) {
        // Windowed frames lay out only the window: other rows' boxes are
        // an older layout's and may still claim this y.
        if (!last_assembly_full_ && (i < last_window_first_ || i > last_window_last_)) continue;
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
        // Same keep-visible rule as ToggleCollapseFocused.
        force_include_next_.insert(key);
        return true;
    }
    return false;
}

bool ChatView::CancelPendingAt(int screen_y) {
    if (!ViewportContainsY(screen_y)) return false;   // see ToggleCollapseAt
    // One box per pending row (the wrapper wraps row + queued tag), parallel
    // to the turn ids. A click consumes only when the row's turn id has
    // arrived -- before TurnQueuedEvent the cancel is not addressable, and
    // consuming the click would just swallow it.
    for (std::size_t i = 0; i < pending_row_boxes_.size() && i < pending_row_turn_ids_.size();
         ++i) {
        const auto& box = pending_row_boxes_[i];
        if (screen_y < box->y_min || screen_y > box->y_max) continue;
        const auto turn_id = pending_row_turn_ids_[i];
        if (turn_id == 0 || !cancel_pending_) return false;
        cancel_pending_(turn_id);
        return true;
    }
    return false;
}

}  // namespace minimal::client
