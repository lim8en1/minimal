#include "minimal/client/view/frame_renderer.hpp"

#include <string_view>

#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"

#include <ftxui/screen/terminal.hpp>

namespace minimal::client {

namespace {

// The {activity} status segment: short text for "is the model doing
// something right now", distinct from the tab bar's glyph (tab_bar.cpp's
// StateGlyph) -- this is the FOCUSED session's detail, spelled out, where the
// glyph is a compact per-tab signal. Idle/unknown renders nothing, same rule
// every other segment follows (an empty value = the segment isn't shown).
//
// Reads the LIGHT per-tab snapshot (TabSnapshot), not deps_.focused_details
// (the DEEP snapshot): focused_details is TabMaintenance's CACHED output,
// refreshed on its own schedule, not necessarily every render -- state and
// tool name are exactly the "cheap facts" TakeLightSnapshot exists for
// (fresh every single Render() call, the same source the tab bar's own
// live-updating glyph already reads), so sourcing this from the deep
// snapshot would have shown a stale activity a frame or more behind the
// glyph right next to it.
std::string ActivityLabel(SessionStateKind state,
                          const std::vector<ToolExecutionState>& tools) {
    switch (state) {
        case SessionStateKind::Thinking:
            return "thinking…";
        case SessionStateKind::Working:
            // The gate-order-first tool, not necessarily the one that
            // finishes first (parallel dispatch) -- good enough as "what's
            // running" orientation, not a live ticker.
            if (!tools.empty() && !tools.front().tool_name.empty()) {
                return "running " + tools.front().tool_name + "…";
            }
            return "working…";
        case SessionStateKind::Waiting:
            return "waiting";
        case SessionStateKind::Error:
            return "error";
        case SessionStateKind::Done:
        case SessionStateKind::Idle:
            return "";
    }
    return "";
}

}  // namespace

FrameRenderer::FrameRenderer(Dependencies deps) : deps_(std::move(deps)) {}

ftxui::Elements FrameRenderer::RenderStartViews(const std::string& session_id) {
    using namespace ftxui;
    Elements out;
    for (const auto& view : deps_.start_view_registry.views()) {
        try {
            out.push_back(view->Render(session_id, deps_.theme) | center);
        } catch (const std::exception& error) {
            common::DefaultLogger().error("start view '{}' threw: {}",
                                          std::string(view->spec().id), error.what());
        } catch (...) {
            common::DefaultLogger().error("start view '{}' threw a non-standard exception",
                                          std::string(view->spec().id));
        }
    }
    return out;
}

ftxui::Element FrameRenderer::Render() {
    using namespace ftxui;

    // Tool output (the panel's live tail) only when the panel is actually
    // about to render -- the tab bar below needs name/state only, and this
    // call already happens every render.
    const auto tabs = deps_.app.GetAllTabs(/*include_tool_output=*/deps_.show_panel);

    // The snapshot (and everything derived from it) was taken by
    // TabMaintenance::Run() on this loop iteration's event, BEFORE Render --
    // never here: this method must stay side-effect-free and non-blocking
    // with respect to RPC/network work. deps_.focused_details is
    // TabMaintenance's cached output.

    // Below this, the tab bar and notice line drop out entirely so the
    // editor and status bar (kept out of ftxui's flex-shrink set below)
    // always keep their rows instead of the vbox shrink pass squeezing a
    // fixed-size row to 0/negative height and drawing the next row on top of
    // it.
    constexpr int kCriticalHeight = 8;
    const int term_height = ftxui::Terminal::Size().dimy;
    const bool space_critical = term_height > 0 && term_height < kCriticalHeight;

    Elements rows;
    if (!space_critical) rows.push_back(RenderTabBar(tabs, deps_.theme));

    const std::string& focused = deps_.focused;
    const SessionState::Snapshot& focused_details = deps_.focused_details;

    // The frame's DEEP copy: the FOCUSED tab only. (The tools panel reads
    // the light TabSnapshot.tools; nothing renders other tabs' heavy data.)
    if (!focused_details.session_id.empty() || !focused.empty()) {
        const auto& snap = focused_details;
        TabUi& ui = deps_.tab_ui[focused];

        // The tab body: the exclusive chat surface when a plugin claimed
        // the slot, the core chat view otherwise. The surface's component
        // is fetched once per tab and cached; the surface's own OnEvent
        // push carries its data (see DispatchToTab). EffectiveChatSurface
        // returns null once a surface has thrown -- the core view takes
        // over and the stale surface_body cache is dropped.
        plugins::ChatSurface* effective_surface = deps_.app.EffectiveChatSurface();
        if (effective_surface != nullptr) {
            if (!ui.surface_body || ui.surface_for != effective_surface) {
                ui.surface_body = effective_surface->component();
                ui.surface_for = effective_surface;
            }
            try {
                rows.push_back(ui.surface_body->Render() | yframe | vscroll_indicator | flex);
            } catch (const std::exception& error) {
                deps_.app.DisableChatSurface(error.what());
                rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                    "takes over") | color(deps_.theme.ColorFor("error")));
            } catch (...) {
                deps_.app.DisableChatSurface(nullptr);
                rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                    "takes over") | color(deps_.theme.ColorFor("error")));
            }
        } else {
            // A FRESH session -- nothing persisted, nothing streaming, no
            // earlier pages -- renders the start-view banner in the tab
            // body instead of an empty transcript. has_earlier_messages
            // keeps a resumed session from flashing the banner during the
            // async scrollback window before its first page applies.
            const bool fresh_session =
                (!snap.scrollback_page || snap.scrollback_page->empty()) &&
                snap.streaming_text.empty() && !snap.has_earlier_messages;
            Elements start = fresh_session ? RenderStartViews(focused) : Elements{};
            if (!start.empty()) {
                for (auto& element : start) rows.push_back(std::move(element) | flex);
            } else {
                // ui.chat/ui.streaming are normally built by
                // TabMaintenance::Maintain (Part 3.2), but Maintain() returns
                // EARLY -- skipping that -- whenever GetDetails raced a tab
                // that just closed (snap.session_id empty) or a chat-surface
                // plugin currently owns the tab; a frame landing here with no
                // start view registered (so `start` stayed empty) then found
                // one or both still null on its very first render. Lazily
                // built here too, so neither is ever a null deref regardless
                // of which path built it.
                if (!ui.chat) {
                    ui.chat = std::make_unique<ChatView>(deps_.theme, deps_.message_registry,
                                                         deps_.renderer_registry);
                }
                if (!ui.streaming) ui.streaming = deps_.engine.MakeStream();
                rows.push_back(ui.chat->Render(snap, *ui.streaming) | yframe |
                               vscroll_indicator | flex);
            }
        }

        // The transient notice line: the focused tab's latest /-command
        // outcome, routed by its CommandDisplay mode (EXTENSIBILITY §4.4).
        // NEVER a chat row -- that is a type-level property of
        // CommandNotice. VIEW side-effects ran in TabMaintenance::
        // HandleNotice, once per notice; here a View notice renders nothing
        // (the view it opened is the surface), and STATUS/errors render as
        // the persistent line until the next result replaces them.
        if (!space_critical && snap.latest_notice.has_value()) {
            const auto& notice = *snap.latest_notice;
            if (notice.display != v1::COMMAND_DISPLAY_VIEW &&
                notice.display != v1::COMMAND_DISPLAY_SILENT) {
                // One text() per line, not one text() over the whole
                // (possibly multi-line) message: text() declares itself ONE
                // row tall regardless of embedded '\n's, so a multi-line
                // notice (e.g. /help's listing) drew past its allotted row
                // and overwrote whatever the vbox placed below it (the tools
                // panel, the editor). A vbox of per-line rows reports its
                // TRUE height, and the size()+yframe below caps and scrolls
                // it instead of letting it dominate the frame.
                Elements notice_lines;
                std::string_view remaining(notice.message);
                std::size_t pos = 0;
                while (pos < remaining.size()) {
                    const std::size_t newline = remaining.find('\n', pos);
                    const std::size_t end =
                        newline == std::string_view::npos ? remaining.size() : newline;
                    notice_lines.push_back(text(" " + std::string(remaining.substr(pos, end - pos))));
                    if (newline == std::string_view::npos) break;
                    pos = newline + 1;
                }
                // size AFTER yframe (not the reverse): Size::SetBox clamps
                // the content box to the cap while the frame's requirement
                // min_y shrinks to the cap, so with size FIRST the frame
                // believes "nothing to scroll" and content below the cap is
                // silently clipped -- a long notice (e.g. /help's listing)
                // showed only its head. yframe-first keeps the full height
                // inside the frame, so the cap is a viewport, not a guillotine.
                rows.push_back(vbox(std::move(notice_lines)) | yframe |
                               size(HEIGHT, LESS_THAN, 6) | vscroll_indicator |
                               bgcolor(deps_.theme.ColorFor("status_bar_bg")) |
                               color(notice.is_error ? deps_.theme.ColorFor("error")
                                                     : deps_.theme.ColorFor("system_message")));
            }
        }
    } else {
        Elements start = RenderStartViews("");
        if (start.empty()) {
            rows.push_back(vbox({text("minimal") | bold,
                                 text("ctrl+o opens the session picker; typing starts a "
                                      "session") |
                                     dim}) |
                           center | flex);
        } else {
            for (auto& element : start) rows.push_back(std::move(element) | flex);
        }
    }

    if (deps_.show_panel) {
        // vscroll_indicator: every other scrollable yframe region in the
        // frame (chat, plugin panels below, the pickers) has one; this was
        // the one silent exception.
        rows.push_back(RenderToolsPanel(tabs, deps_.tools_panel_stdout_buffer_lines, deps_.theme) |
                       yframe | size(HEIGHT, LESS_THAN, deps_.panel_height) | vscroll_indicator);
    }

    // Plugin PANEL views, beside the tools panel -- moved here (this used to
    // sit after the status bar, rendering every plugin panel BELOW it,
    // contradicting this very comment) so they actually land next to the
    // tools panel they're described as beside. Events reach the visible ones
    // through the frame's CatchEvent (see with_keys). A throwing view is
    // hidden for the frame that threw (never fatal), like the status
    // segments below.
    for (auto& entry : deps_.plugin_views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible) {
            try {
                rows.push_back(entry.component->Render() |
                               yframe | size(HEIGHT, LESS_THAN, deps_.panel_height));
            } catch (const std::exception& error) {
                common::DefaultLogger().error("plugin view '{}' threw: {}",
                                              std::string(entry.view->spec().id), error.what());
            } catch (...) {
                common::DefaultLogger().error(
                    "plugin view '{}' threw a non-standard exception",
                    std::string(entry.view->spec().id));
            }
        }
    }

    rows.push_back(deps_.editor->Render());

    // Status bar. model/agent/permission_mode render from focused_details
    // below (the E5 metadata emission); empty/unknown renders as nothing,
    // per the format string's unknown-or-unfilled rule.
    std::map<std::string, std::string> values;
    values["version"] = std::string(common::Version());
    // The focused tab's DEEP snapshot (see focused_details above) drives
    // the status bar's values.
    if (!focused_details.session_id.empty()) {
        values["session"] = focused_details.display_name;
        // The interrupt press is never silent: from SendCancel until the
        // turn actually ends, the focused session says so (status bar =
        // the home of focused-session detail, per ARCHITECTURE.md).
        if (focused_details.cancel_requested) {
            values["session"] += "  ·  cancelling…";
        }
        // {tokens} is used/max now that the metadata emission carries the
        // window (E5a); max unknown (0) renders used-only, the E4 behavior.
        const int64_t used = focused_details.input_tokens + focused_details.output_tokens;
        if (used > 0) {
            values["tokens"] = focused_details.context_window_tokens > 0
                                   ? std::to_string(used) + "/" +
                                         std::to_string(focused_details.context_window_tokens) +
                                         " tok"
                                   : std::to_string(used) + " tok";
        }
        // The metadata segments: empty = unknown = render nothing, the
        // format string's unknown-or-unfilled rule.
        if (!focused_details.model.empty()) values["model"] = focused_details.model;
        if (!focused_details.agent_preset.empty()) values["agent"] = focused_details.agent_preset;
        if (!focused_details.permission_mode.empty()) {
            values["permission_mode"] = focused_details.permission_mode;
        }
        for (const auto& tab : tabs) {
            if (tab.session_id != focused) continue;
            const auto activity = ActivityLabel(tab.state, tab.tools);
            if (!activity.empty()) values["activity"] = activity;
            break;
        }
    }
    // Registered segment providers fill their own ids. Render runs on the
    // render thread at ~20 Hz -- the provider contract (cheap, never
    // blocking) is stated on the interface; a THROW is contained (that
    // segment renders nothing for this frame), never fatal.
    for (const auto& segment : deps_.status_registry.segments()) {
        try {
            values[std::string(segment->id())] = segment->Render(focused);
        } catch (const std::exception& error) {
            common::DefaultLogger().error("status segment '{}' threw: {}",
                                          std::string(segment->id()), error.what());
        } catch (...) {
            common::DefaultLogger().error("status segment '{}' threw a non-standard "
                                          "exception", std::string(segment->id()));
        }
    }
    rows.push_back(RenderStatusBar(deps_.status_format, values, deps_.theme,
                                   ftxui::Terminal::Size().dimx));

    return vbox(std::move(rows));
}

}  // namespace minimal::client
