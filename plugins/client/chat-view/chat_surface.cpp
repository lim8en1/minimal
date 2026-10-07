#include "chat_surface.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::client::chat_view {

namespace {

constexpr const char* kFoldThinkingKey = "fold_thinking";
constexpr const char* kFoldToolsKey = "fold_tools";

const char* FoldName(int fold) {
    return fold == kFoldCollapsed ? "collapsed" : fold == kFoldExpanded ? "expanded" : "auto";
}

std::optional<int> FoldFrom(const std::string& name) {
    if (name == "auto") return kFoldDefaultAuto;
    if (name == "expanded") return kFoldExpanded;
    if (name == "collapsed") return kFoldCollapsed;
    return std::nullopt;
}

}  // namespace

SurfaceConfig ParseSurfaceConfig(const nlohmann::json& config) {
    SurfaceConfig out;
    minimal::config::Reader reader(
        config, minimal::common::PluginConfigLabel(minimal::common::kClientPluginConfigDirName,
                                                   "chat-view"));
    for (const auto& [key, field] : {std::pair{kFoldThinkingKey, &out.fold_thinking},
                                     std::pair{kFoldToolsKey, &out.fold_tools}}) {
        const std::string name = reader.String(key, "auto");
        const auto fold = FoldFrom(name);
        if (!fold) {
            throw std::runtime_error(std::string("chat-view: ") + key +
                                     " must be \"auto\", \"expanded\" or \"collapsed\"");
        }
        *field = *fold;
    }
    reader.Finish();
    return out;
}

// --- Surface --------------------------------------------------------------------

Surface::Surface(SurfaceConfig config, plugins::HostUi* ui)
    : ui_(ui), fold_thinking_(config.fold_thinking), fold_tools_(config.fold_tools) {
    // A toggle saved last session wins over the configured start.
    const nlohmann::json state = ui_ != nullptr ? ui_->LoadState() : nlohmann::json();
    if (state.is_object()) {
        if (const auto fold = FoldFrom(state.value(kFoldThinkingKey, ""))) fold_thinking_ = *fold;
        if (const auto fold = FoldFrom(state.value(kFoldToolsKey, ""))) fold_tools_ = *fold;
    }
}

std::vector<plugins::DeclaredAction> Surface::Actions() const {
    return {
        // Not ctrl+up/down: macOS takes those systemwide. ctrl+alt+k/j pair
        // with the row cursor's alt+k/j as their coarser variant.
        {"chat.scroll_up", "ctrl+alt+k", "scroll up"},
        {"chat.scroll_down", "ctrl+alt+j", "scroll down"},
        // Not "end": global keys run before the editor, which needs End.
        // PageDown pairs with chat.load_older's PageUp.
        {"chat.scroll_bottom", "pagedown", "jump to the newest message"},
        // Not shift+up/down: no distinct encoding; alt+j/k are.
        {"chat.row_up", "alt+k", "row cursor up"},
        {"chat.row_down", "alt+j", "row cursor down"},
        {"chat.toggle_collapse", "alt+c", "expand / collapse the row"},
        // Not alt+shift+c: alt drops shift on a letter, colliding with alt+c.
        {"chat.toggle_collapse_all", "ctrl+alt+c", "expand / collapse every row"},
        // The fold preferences: kept across restarts (the plugin's state).
        {"chat.toggle_thinking_fold", "ctrl+alt+t", "auto-fold / unfold thinking rows"},
        {"chat.toggle_tool_fold", "ctrl+alt+o", "auto-fold / unfold tool rows"},
        {"chat.toggle_timestamps", "alt+t", "show / hide timestamps"},
    };
}

std::unique_ptr<plugins::ChatPane> Surface::Make(plugins::ChatHost& host, const std::string& tab_id,
                                                 const Theme& theme) {
    return std::make_unique<Pane>(*this, host, tab_id, theme);
}

void Surface::Attach(Pane* pane) { panes_.push_back(pane); }

void Surface::Detach(Pane* pane) { std::erase(panes_, pane); }

void Surface::ToggleFold(Pane& origin, bool thinking) {
    // A preference for every tab: each one flips (they agree), tabs opened
    // later start from it, and it is kept for the next launch.
    int next = thinking ? fold_thinking_ : fold_tools_;
    for (Pane* pane : panes_) {
        next = thinking ? pane->view().ToggleThinkingFold() : pane->view().ToggleToolFold();
    }
    (thinking ? fold_thinking_ : fold_tools_) = next;
    bool saved = false;
    if (ui_ != nullptr) {
        ui_->SaveState({{kFoldThinkingKey, FoldName(fold_thinking_)},
                        {kFoldToolsKey, FoldName(fold_tools_)}});
        saved = true;
    }
    origin.Notice(std::string(thinking ? "thinking rows: " : "tool rows: ") +
                  (next == kFoldCollapsed ? "collapsed" : "expanded") +
                  (saved ? " (saved)" : ""));
}

// --- Pane -----------------------------------------------------------------------

Pane::Pane(Surface& surface, plugins::ChatHost& host, std::string tab_id, const Theme& theme)
    : surface_(surface),
      host_(host),
      tab_id_(std::move(tab_id)),
      theme_(theme),
      view_(theme, host.MessageRenderers(), host.ToolRenderers(), host.DisplayRenderers(),
            host.KeyFor("chat.toggle_collapse"), host.KeyFor("chat.load_older")) {
    view_.SetFoldDefaults(surface_.fold_thinking(), surface_.fold_tools());
    // Click-to-cancel for a QUEUED message row of this tab.
    view_.SetCancelPendingHandler(
        [this](std::uint64_t turn_id) { host_.CancelQueued(tab_id_, turn_id); });
    surface_.Attach(this);
}

Pane::~Pane() { surface_.Detach(this); }

void Pane::Notice(const std::string& text) { host_.Notice(tab_id_, text); }

ftxui::Element Pane::Render(const plugins::ChatFrame& frame) {
    using namespace ftxui;
    MaintainStreams(frame);
    // The view wraps its own ScrollRegion (explicit line-offset scrolling);
    // flex is the only outer decoration.
    Element body = view_.Render(frame, streams_) | flex;
    // Scrolled up (or parked on a row) while the turn is still producing,
    // or after output landed below the viewport: nothing else says so.
    if (!view_.IsScrolledAway() || !(frame.turn_busy || view_.HasUnseenBelow())) return body;
    // The key only when one is bound: an unbound action read " ·  to follow".
    const std::string key = host_.KeyFor("chat.scroll_bottom");
    return vbox({std::move(body),
                 text(" ↓ new output below" + (key.empty() ? "" : " · " + key + " to follow")) |
                     color(theme_.ColorFor("attention"))});
}

void Pane::OnAction(std::string_view action) {
    if (action == "chat.scroll_up") view_.ScrollBy(1);   // toward older
    else if (action == "chat.scroll_down") view_.ScrollBy(-1);
    else if (action == "chat.scroll_bottom") view_.ScrollToBottom();
    else if (action == "chat.row_up") view_.MoveRowFocus(1);   // toward older
    else if (action == "chat.row_down") view_.MoveRowFocus(-1);
    else if (action == "chat.toggle_collapse") view_.ToggleCollapseFocused();
    else if (action == "chat.toggle_collapse_all") view_.ToggleCollapseAll();
    else if (action == "chat.toggle_timestamps") view_.ToggleTimestamps();
    else if (action == "chat.toggle_thinking_fold") surface_.ToggleFold(*this, /*thinking=*/true);
    else if (action == "chat.toggle_tool_fold") surface_.ToggleFold(*this, /*thinking=*/false);
}

bool Pane::OnMouse(const ftxui::Event& event) {
    using ftxui::Mouse;
    ftxui::Event copy = event;   // mouse() is not const-qualified upstream
    const auto& mouse = copy.mouse();
    if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
        // Only over the chat: the wheel over a panel or the editor is theirs.
        if (!view_.ViewportContainsY(mouse.y)) return false;
        // WheelUp reveals older content. One tick per event: the view turns
        // it into a few lines and gates the momentum flood.
        view_.ScrollWheelBy(mouse.button == Mouse::WheelUp ? 1 : -1);
        return true;
    }
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
        // A queued message row cancels it; a collapsible row toggles. A miss
        // falls through (text selection, the editor).
        return view_.CancelPendingAt(mouse.y) || view_.ToggleCollapseAt(mouse.y);
    }
    return false;
}

void Pane::OnSubmitted() {
    // The message is about to show at the bottom: follow it, and drop a row
    // cursor left over from browsing.
    view_.ScrollToBottom();
    view_.ClearRowFocus();
}

void Pane::MaintainStreams(const plugins::ChatFrame& frame) {
    const auto make_stream = [&] { return host_.Markdown().MakeStream(); };

    // One stream slot per live TEXT segment (track C4), keyed by the
    // segment's stable id (ChatLiveSegment::segment_id) -- NOT its vector
    // index. The old index-aligned vector assumed "segments only ever APPEND
    // within a turn", but three paths REMOVE without bumping turn_seq:
    // LiveTurnBuffer::Retire (a mid-turn page apply), the count cap's
    // trim, and ClearStreamingIfCurrentTurn. The survivors shift down, an
    // index-keyed slot's `seen` watermark then named a DIFFERENT segment,
    // and AppendDelta(text.substr(seen)) spliced the tail of one segment
    // onto another segment's buffered view -- corrupted for the rest of the
    // turn ("random unrelated text" at the end of long thinking blocks;
    // self-healing only because the next turn's turn_seq bump cleared the
    // whole vector). Ids are never reused, and removals are permanent
    // within a turn, so a slot whose id is absent from this pass's snapshot
    // is dead -- the GC below retires it, which also replaces the old
    // wholesale turn-boundary clear.
    std::vector<uint64_t> live_text_ids;
    live_text_ids.reserve(frame.live_segments.size());
    for (const auto& segment : frame.live_segments) {
        if (segment.kind != plugins::ChatLiveSegment::Kind::Text) continue;
        live_text_ids.push_back(segment.segment_id);
        LiveTextStreamSlot* slot = nullptr;
        for (auto& candidate : streams_) {
            if (candidate.segment_id == segment.segment_id) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            streams_.push_back(
                LiveTextStreamSlot{.segment_id = segment.segment_id});
            slot = &streams_.back();
        }
        // The snapshot's copy-on-publish view (state.hpp LiveTurnSegment) --
        // shared with every other consumer of this segment's current text;
        // only the actively-appending segment republishes per delta.
        const std::string& text = *segment.text;
        // A plugin MarkdownEngine is not trusted to return normally: a
        // throw here used to kill the whole TUI mid-turn. MakeStream()
        // throwing leaves slot.view null -- the guard above naturally
        // retries next pass, since nothing below has consumed `text` yet.
        if (!slot->view) {
            try {
                slot->view = make_stream();
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown engine MakeStream threw: {}",
                                             error.what());
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown engine MakeStream threw a non-standard exception");
            }
        }
        if (slot->view && text.size() > slot->seen) {
            try {
                slot->view->AppendDelta(std::string_view(text).substr(slot->seen));
                slot->seen = text.size();
                // New text on an ALREADY-finalized segment means a re-arm at
                // the engine level (the engine stream's AppendDelta, track
                // A1) -- mirror that here too, so this segment gets its own
                // fresh authoritative reparse when it next finalizes, exactly
                // like the engine's own re-arm.
                slot->finalized_for_turn = false;
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown stream AppendDelta threw: {}",
                                             error.what());
                // Discard the (possibly now-corrupted) stream rather than
                // keep feeding it every frame; slot.seen resets too, so the
                // fresh stream MakeStream() builds next pass replays the
                // FULL accumulated text from scratch instead of missing the
                // span this call was trying to append.
                slot->view.reset();
                slot->seen = 0;
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown stream AppendDelta threw a non-standard exception");
                slot->view.reset();
                slot->seen = 0;
            }
        }
    }
    // GC: slots whose segment is gone. Safe against a transient absence
    // because segments never reappear: every removal path (retirement,
    // trim, turn-aware clear) is permanent, and ids are never reused.
    streams_.erase(
        std::remove_if(streams_.begin(), streams_.end(),
                       [&](const LiveTextStreamSlot& slot) {
                           return std::find(live_text_ids.begin(), live_text_ids.end(),
                                            slot.segment_id) == live_text_ids.end();
                       }),
        streams_.end());
    // The MOST RECENT text segment's authoritative reparse (the engine
    // contract: Finalize replaces artifacts from mid-stream unclosed spans).
    // is_streaming_finalized fires once per BLOCK boundary, and blocks are
    // strictly sequential (never interleaved with each other -- only with
    // tool activity, which is what live_segments itself now records), so it
    // always names the LAST text segment in the timeline; an earlier one
    // already got its own Finalize when ITS block ended.
    if (frame.streaming_finalized) {
        for (std::size_t i = frame.live_segments.size(); i-- > 0;) {
            if (frame.live_segments[i].kind != plugins::ChatLiveSegment::Kind::Text) continue;
            LiveTextStreamSlot* slot = nullptr;
            for (auto& candidate : streams_) {
                if (candidate.segment_id == frame.live_segments[i].segment_id) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot != nullptr && slot->view && !slot->finalized_for_turn) {
                try {
                    slot->view->Finalize();
                    slot->finalized_for_turn = true;
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("markdown stream Finalize threw: {}",
                                                 error.what());
                    slot->view.reset();
                    slot->seen = 0;
                } catch (...) {
                    common::DefaultLogger().warn(
                        "markdown stream Finalize threw a non-standard exception");
                    slot->view.reset();
                    slot->seen = 0;
                }
            }
            break;
        }
    }

}

}  // namespace minimal::client::chat_view
