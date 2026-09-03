#include "minimal/client/view/frame_renderer.hpp"

#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

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

    Elements rows;
    rows.push_back(RenderTabBar(tabs, deps_.theme));

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
        if (snap.latest_notice.has_value()) {
            const auto& notice = *snap.latest_notice;
            if (notice.display != v1::COMMAND_DISPLAY_VIEW &&
                notice.display != v1::COMMAND_DISPLAY_SILENT) {
                rows.push_back(text(" " + notice.message) |
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
        rows.push_back(RenderToolsPanel(tabs, deps_.tools_panel_stdout_buffer_lines, deps_.theme) |
                       size(HEIGHT, LESS_THAN, deps_.panel_height) | yframe);
    }

    rows.push_back(deps_.editor->Render() | flex);

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
    }
    // Registered segment providers fill their own ids. Render runs on the
    // render thread at ~20 Hz -- the provider contract (cheap, never
    // blocking) is stated on the interface; a THROW is contained (that
    // segment renders nothing for this frame), never fatal.
    for (const auto& segment : deps_.status_registry.segments()) {
        try {
            values[std::string(segment->id())] = segment->Render();
        } catch (const std::exception& error) {
            common::DefaultLogger().error("status segment '{}' threw: {}",
                                          std::string(segment->id()), error.what());
        } catch (...) {
            common::DefaultLogger().error("status segment '{}' threw a non-standard "
                                          "exception", std::string(segment->id()));
        }
    }
    rows.push_back(RenderStatusBar(deps_.status_format, values, deps_.theme));

    // Plugin PANEL views, beside the tools panel. Events reach the visible
    // ones through the frame's CatchEvent (see with_keys). A throwing view
    // is hidden for the frame that threw (never fatal), like the status
    // segments.
    for (auto& entry : deps_.plugin_views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible) {
            try {
                rows.push_back(entry.component->Render() |
                               size(HEIGHT, LESS_THAN, deps_.panel_height) | yframe);
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

    return vbox(std::move(rows));
}

}  // namespace minimal::client
