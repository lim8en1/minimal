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

    // LIGHT only: the tab bar and the activity-label lookup below need
    // name/state/tool-name, never buffered tool output. The tools panel
    // (which DOES need it) gets its own, separately cached, heavy fetch
    // below -- gated on UiVersion so it isn't redone on every render (FTXUI
    // renders on every event, including mouse move, not just on a real
    // state change).
    const auto tabs = deps_.app.GetAllTabs(/*include_tool_output=*/false);

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
        // find(), not operator[]: Render() is documented side-effect-free, and
        // operator[] would default-create a TabUi for any focused id Maintain
        // has not seen yet -- a silent mutation of shared state from the
        // render path.
        const auto ui_it = deps_.tab_ui.find(focused);
        if (ui_it == deps_.tab_ui.end()) return emptyElement();
        TabUi& ui = ui_it->second;

        // The tab body: the exclusive chat surface when a plugin claimed
        // the slot, the core chat view otherwise. The surface's component
        // is fetched once per tab and cached; the surface's own OnEvent
        // push carries its data (see DispatchToTab). EffectiveChatSurface
        // returns null once a surface has thrown -- the core view takes
        // over and the stale surface_body cache is dropped.
        plugins::ChatSurface* effective_surface = deps_.app.EffectiveChatSurface();
        if (effective_surface != nullptr) {
            if (!ui.surface_body || ui.surface_for != effective_surface) {
                // component() is plugin code like the Render() below: its
                // throw used to escape the frame and std::terminate the TUI.
                // Same containment -- the surface is disabled (the core chat
                // view takes over next frame), and a body that never arrived
                // simply skips this frame's body row.
                try {
                    ui.surface_body = effective_surface->component();
                    ui.surface_for = effective_surface;
                } catch (const std::exception& error) {
                    deps_.app.DisableChatSurface(error.what());
                } catch (...) {
                    deps_.app.DisableChatSurface(nullptr);
                }
            }
            if (ui.surface_body != nullptr && ui.surface_for == effective_surface) {
                try {
                    rows.push_back(ui.surface_body->Render() | yframe | vscroll_indicator |
                                   flex);
                } catch (const std::exception& error) {
                    deps_.app.DisableChatSurface(error.what());
                    rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                        "takes over") | color(deps_.theme.ColorFor("error")));
                } catch (...) {
                    deps_.app.DisableChatSurface(nullptr);
                    rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                        "takes over") | color(deps_.theme.ColorFor("error")));
                }
            }
        } else {
            // A FRESH session -- nothing persisted, nothing streaming, no
            // earlier pages -- renders the start-view banner in the tab
            // body instead of an empty transcript. has_earlier_messages
            // keeps a resumed session from flashing the banner during the
            // async scrollback window before its first page applies.
            //
            // pending_user_text/busy keep the banner off too: a FRESH session
            // mid-turn -- especially a tool-calling turn whose model has not
            // emitted any text yet -- still has things to show (the echoed
            // prompt, live tool rows), and the banner hid exactly those
            // (submit -> banner until the first text delta) .
            const bool turn_busy = snap.state == SessionStateKind::Thinking ||
                                   snap.state == SessionStateKind::Working ||
                                   snap.state == SessionStateKind::Waiting;
            const bool fresh_session =
                (!snap.scrollback_page || snap.scrollback_page->empty()) &&
                snap.streaming_text.empty() && !snap.has_earlier_messages &&
                snap.pending_user_text.empty() && !turn_busy;
            // Start views are SESSION-keyed like the status segments (a
            // checklist/cost view caches per session) -- the frame's focused
            // handle is a TAB id, so the snapshot's session id goes out
            // (empty = no tab / sessionless). Passing the tab id used to
            // make plugin start views read the wrong cache key.
            Elements start =
                fresh_session ? RenderStartViews(focused_details.session_id) : Elements{};
            if (!start.empty()) {
                for (auto& element : start) rows.push_back(std::move(element) | flex);
            } else {
                // ui.chat is normally built by TabMaintenance::Maintain
                // (Part 3.2), but Maintain() returns EARLY -- skipping that
                // -- whenever GetDetails raced a tab that just closed
                // (snap.session_id empty) or a chat-surface plugin currently
                // owns the tab; a frame landing here with no start view
                // registered (so `start` stayed empty) then found it still
                // null on its very first render. Lazily built here too, so
                // it is never a null deref regardless of which path built
                // it. ui.live_text_streams needs no such fallback (track
                // C4): ChatView::Render defensively skips any live_segments
                // index it has no slot for, so an empty (or short) vector
                // here just means "nothing live rendered yet" -- exactly
                // right for this same race, not a null-deref risk.
                if (!ui.chat) {
                    ui.chat = std::make_unique<ChatView>(deps_.theme, deps_.message_registry,
                                                         deps_.renderer_registry,
                                                         deps_.display_registry,
                                                         deps_.toggle_collapse_hint);
                }
                rows.push_back(ui.chat->Render(snap, ui.live_text_streams) | yframe |
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
                                     Muted(deps_.theme)}) |
                           center | flex);
        } else {
            for (auto& element : start) rows.push_back(std::move(element) | flex);
        }
    }

    if (deps_.show_panel) {
        // Rebuilt only when something the panel actually shows changed
        // (DispatchToTab bumps UiVersion on every wire event) -- with the
        // panel open, rebuilding unconditionally deep-copied every tab's
        // buffered tool output (up to tool_output_buffer_bytes_ each) under
        // AppState + session locks on every mouse move, blocking reader
        // threads for no reason.
        const uint64_t version = deps_.app.UiVersion();
        if (!tools_panel_cache_valid_ || version != tools_panel_cache_version_) {
            const auto panel_tabs = deps_.app.GetAllTabs(/*include_tool_output=*/true);
            // vscroll_indicator: every other scrollable yframe region in the
            // frame (chat, plugin panels below, the pickers) has one; this
            // was the one silent exception.
            tools_panel_cache_ =
                RenderToolsPanel(panel_tabs, deps_.tools_panel_stdout_buffer_lines, deps_.theme) |
                yframe | size(HEIGHT, LESS_THAN, deps_.panel_height) | vscroll_indicator;
            tools_panel_cache_version_ = version;
            tools_panel_cache_valid_ = true;
        }
        rows.push_back(tools_panel_cache_);
    }

    // Plugin PANEL views, beside the tools panel -- moved here (this used to
    // sit after the status bar, rendering every plugin panel BELOW it,
    // contradicting this very comment) so they actually land next to the
    // tools panel they're described as beside. Events reach the FOCUSED one
    // through the frame's CatchEvent (see with_keys and
    // keybinding_dispatch.cpp); here focus is presentation only: the focused
    // panel gets a border + title chrome, the unfocused ones render dimmed.
    // A throwing view is hidden for the frame that threw (never fatal), like
    // the status segments below.
    for (auto& entry : deps_.plugin_views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible) {
            // A view whose component() factory returned null without
            // throwing (bootstrap's own guard only catches a THROWING
            // component()) would otherwise dereference a null Component here
            // -- a segfault, not a catchable exception, so the try below
            // cannot protect against it.
            if (entry.component == nullptr) continue;
            Element body;
            try {
                body = entry.component->Render();
            } catch (const std::exception& error) {
                common::DefaultLogger().error("plugin view '{}' threw: {}",
                                              std::string(entry.view->spec().id), error.what());
                continue;
            } catch (...) {
                common::DefaultLogger().error(
                    "plugin view '{}' threw a non-standard exception",
                    std::string(entry.view->spec().id));
                continue;
            }

            Element framed = std::move(body) | yframe |
                             size(HEIGHT, LESS_THAN, deps_.panel_height);
            if (entry.focused) {
                // The focused panel's chrome: a title strip over the content,
                // the whole box bordered in the theme's focused-border role.
                // The title rides INSIDE the border (border is applied last),
                // so the cap below bounds content + title + chrome together.
                framed = vbox({
                             text(" " + entry.view->spec().title + " ") |
                                 bgcolor(deps_.theme.ColorFor("status_bar_bg")) | bold,
                             std::move(framed),
                         }) |
                         borderStyled(deps_.theme.ColorFor("border_focused"));
            } else {
                framed = std::move(framed) | Muted(deps_.theme);
            }
            rows.push_back(std::move(framed));
        }
    }

    rows.push_back(deps_.editor->Render());

    // Status bar. model/agent/permission_mode render from focused_details
    // below (the E5 metadata emission); empty/unknown renders as nothing,
    // per the format string's unknown-or-unfilled rule.
    std::map<std::string, std::string> values;
    values["version"] = std::string(common::Version());
    // The focused tab's DEEP snapshot (see focused_details above) drives
    // the status bar's values. A SESSIONLESS tab renders too: it has a name
    // ("new session") and any staged /agent //model choices -- what it lacks
    // is only the server-side metadata, which fills in at EnsureSession.
    if (!focused.empty()) {
        values["session"] = !focused_details.display_name.empty()
                                ? focused_details.display_name
                                : std::string("new session");
        // The interrupt press is never silent: from SendCancel until the
        // turn actually ends, the focused session says so (status bar =
        // the home of focused-session detail, per ARCHITECTURE.md).
        if (focused_details.cancel_requested) {
            values["session"] += "  ·  cancelling…";
        }
        // {tokens} is used/max now that the metadata emission carries the
        // window (E5a); max unknown (0) renders used-only, the E4 behavior.
        // Cache tokens ARE occupancy: providers report input_tokens as the
        // UNCACHED portion (Anthropic's semantics, which provider-openai's
        // ParseUsage normalizes to), so summing input+output alone made the
        // bar under-count heavily cached sessions -- the exact gap
        // llm::context_occupancy (server/llm/token_accounting.cpp) fixed for
        // the compaction trigger. Same four-field sum here, so the bar's
        // used/max and the server's trigger agree.
        const int64_t used = focused_details.input_tokens + focused_details.output_tokens +
                             focused_details.cache_read_tokens +
                             focused_details.cache_write_tokens;
        if (used > 0) {
            values["tokens"] = focused_details.context_window_tokens > 0
                                   ? std::to_string(used) + "/" +
                                         std::to_string(focused_details.context_window_tokens) +
                                         " tok"
                                   : std::to_string(used) + " tok";
        }
        // The metadata segments: empty = unknown = render nothing, the
        // format string's unknown-or-unfilled rule. The {model}/{agent}
        // values show the DESIRED state when one is staged (the user's last
        // word, per the desired-state convergence rule): equal to effective
        // it renders identically, divergent it renders the fresh value with
        // a pending marker until the confirming metadata event lands -- on
        // a sessionless tab there is nothing to converge into, so no
        // marker (the create simply carries the choice).
        const bool model_pending = !focused_details.session_id.empty() &&
                                   !focused_details.desired_model.empty() &&
                                   focused_details.desired_model != focused_details.model;
        const bool agent_pending =
            !focused_details.session_id.empty() &&
            !focused_details.desired_agent_preset.empty() &&
            focused_details.desired_agent_preset != focused_details.agent_preset;
        const std::string model_value =
            !focused_details.desired_model.empty() ? focused_details.desired_model
                                                   : focused_details.model;
        const std::string agent_value = !focused_details.desired_agent_preset.empty()
                                            ? focused_details.desired_agent_preset
                                            : focused_details.agent_preset;
        if (!model_value.empty()) {
            values["model"] = model_value + (model_pending ? "~" : "");
        }
        if (!agent_value.empty()) {
            values["agent"] = agent_value + (agent_pending ? "~" : "");
        }
        if (!focused_details.permission_mode.empty()) {
            values["permission_mode"] = focused_details.permission_mode;
        }
        for (const auto& tab : tabs) {
            if (tab.tab_id != focused) continue;
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
            // Segments are SESSION-keyed (cost-view caches per focused
            // session); the frame's focused handle is a TAB id, so the
            // snapshot's session id goes out (empty = no tab / sessionless).
            values[std::string(segment->id())] = segment->Render(focused_details.session_id);
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
