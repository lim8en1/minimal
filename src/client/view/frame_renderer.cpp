#include "minimal/client/view/frame_renderer.hpp"

#include <chrono>
#include <string_view>

#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
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

bool FrameRenderer::GuardPluginCall(const std::string& key, const std::string& what,
                                    const std::function<void()>& call) {
    int& throws = plugin_throws_[key];
    if (throws < 0) return false;   // disabled after repeated throws
    std::string error;
    try {
        call();
        throws = 0;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "a non-standard exception";
    }
    if (++throws >= kPluginThrowLimit) {
        common::DefaultLogger().error("{} threw {} times in a row ({}); disabled for this session",
                                      what, kPluginThrowLimit, error);
        throws = -1;
    } else if (throws == 1) {
        common::DefaultLogger().error("{} threw: {}", what, error);
    }
    return false;
}

ftxui::Elements FrameRenderer::RenderStartViews(const std::string& session_id) {
    using namespace ftxui;
    Elements out;
    for (const auto& view : deps_.start_view_registry.views()) {
        const std::string id(view->spec().id);
        GuardPluginCall("start:" + id, "start view '" + id + "'", [&] {
            out.push_back(view->Render(session_id, deps_.theme) | center);
        });
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

    // The tab bar renders the FILTERED list (state.hpp's TabFilter -- the
    // user's tab.filter_group/tab.filter_agents choice), while `tabs` above
    // stays the FULL list: the status-bar lookups below, the tools panel's
    // cache key, and the panel itself are cross-session by design. The
    // badge (tab_bar.hpp) surfaces what the filter hides -- most importantly
    // how many hidden tabs carry '!'.
    const TabFilter tab_filter = deps_.app.ActiveTabFilter();
    const auto visible_tabs = FilterTabs(tabs, tab_filter, deps_.focused);
    const auto badge = ComputeTabBarBadge(tabs, visible_tabs, tab_filter);

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
    // One tab: the bar would only repeat what the status bar and the chat
    // already say, for a whole row. It appears with the second tab -- unless
    // a tab filter is active, when the badge alone justifies the row (it
    // names the filter and counts the hidden '!' tabs).
    if (!space_critical && (visible_tabs.size() > 1 || badge.active)) {
        rows.push_back(RenderTabBar(visible_tabs, deps_.theme, badge, &tab_hits_));
    }

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
                    rows.push_back(text(" chat surface plugin threw · the core chat view "
                                        "takes over") | color(deps_.theme.ColorFor("error")));
                } catch (...) {
                    deps_.app.DisableChatSurface(nullptr);
                    rows.push_back(text(" chat surface plugin threw · the core chat view "
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
                !snap.has_streaming_text && !snap.has_earlier_messages &&
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
                // C4): ChatView::Render defensively skips any live segment
                // whose stable id has no slot for, so an empty (or short)
                // vector here just means "nothing live rendered yet" --
                // exactly right for this same race, not a null-deref risk.
                if (!ui.chat && deps_.ensure_chat_view) deps_.ensure_chat_view(focused, ui);
                // ChatView::Render wraps its own ScrollRegion + vscroll_
                // indicator (explicit line-offset scrolling -- see
                // chat_scroll_region.hpp); yframe's focus-driven scroll
                // would fight it. flex is the only outer decoration left.
                rows.push_back(ui.chat->Render(snap, ui.live_text_streams) | flex);
                // Scrolled up (or parked on a row) while the turn is still
                // producing, or after output already landed below the
                // viewport: nothing else says so. Outside the yframe, so it
                // stays on screen.
                const bool new_output_below =
                    ui.chat->IsScrolledAway() && (turn_busy || ui.chat->HasUnseenBelow());
                if (new_output_below) {
                    rows.push_back(text(" ↓ new output below · " + deps_.scroll_bottom_hint +
                                        " to follow") |
                                   color(deps_.theme.ColorFor("attention")));
                }
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
                    // paragraph(), not text(): a line wider than the
                    // terminal wraps inside the capped viewport instead of
                    // losing its tail at the edge -- notices carry error
                    // messages whose END is often the useful part.
                    notice_lines.push_back(
                        paragraph(" " + std::string(remaining.substr(pos, end - pos))));
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
                                 text(deps_.session_picker_hint +
                                      " opens the session picker; typing starts a session") |
                                     Muted(deps_.theme)}) |
                           center | flex);
        } else {
            for (auto& element : start) rows.push_back(std::move(element) | flex);
        }
    }

    if (deps_.show_panel) {
        // Rebuilt only when something the panel actually shows changed --
        // keyed on ToolsVersion/tab-count/theme-epoch, NOT UiVersion (which
        // moves on every text delta; see frame_renderer.hpp for the key's
        // rationale). With the key matching, the fetch is still cheap: the
        // per-tool output views are copy-on-publish shared pointers.
        bool any_running = false;
        for (const auto& snap : tabs) {
            if (snap.state == SessionStateKind::Working) {
                any_running = true;
                break;
            }
        }
        const std::int64_t elapsed_bucket =
            any_running ? std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count()
                        : 0;
        const PanelCacheKey key{deps_.app.ToolsVersion(), tabs.size(), deps_.theme.epoch(),
                                 elapsed_bucket};
        if (!tools_panel_cache_valid_ || !(key == tools_panel_cache_key_)) {
            const auto panel_tabs = deps_.app.GetAllTabs(/*include_tool_output=*/true);
            // vscroll_indicator: every other scrollable yframe region in the
            // frame (chat, plugin panels below, the pickers) has one; this
            // was the one silent exception.
            tools_panel_cache_ =
                RenderToolsPanel(panel_tabs, deps_.tools_panel_stdout_buffer_lines, deps_.theme) |
                yframe | size(HEIGHT, LESS_THAN, deps_.panel_height) | vscroll_indicator;
            tools_panel_cache_key_ = key;
            tools_panel_cache_valid_ = true;
        }
        rows.push_back(tools_panel_cache_);
    }

    // Plugin PANEL views, beside the tools panel -- moved here (this used to
    // sit after the status bar, rendering every plugin panel BELOW it,
    // contradicting this very comment) so they actually land next to the
    // tools panel they're described as beside. Events reach the FOCUSED one
    // through the frame's CatchEvent (see with_keys and
    // keybinding_dispatch.cpp); here focus is presentation only: every panel
    // is framed and titled, the focused one's border in the focus color, the
    // unfocused ones' content muted.
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
            const std::string id(entry.view->spec().id);
            if (!GuardPluginCall("panel:" + id, "plugin view '" + id + "'",
                                 [&] { body = entry.component->Render(); })) {
                continue;
            }

            Element framed = std::move(body) | yframe |
                             size(HEIGHT, LESS_THAN, deps_.panel_height);
            // EVERY panel gets the same chrome -- a title strip inside a
            // border -- so focusing one never shifts the layout (the border
            // used to appear only on focus: two rows and two columns of jump)
            // and an unfocused panel still says which plugin it is. Focus is
            // the border's color and the title's weight; unfocused content is
            // muted. The title rides INSIDE the border, so the cap above
            // bounds content only and the chrome is a constant 3 rows.
            Element title = text(" " + entry.view->spec().title + " ") |
                            bgcolor(deps_.theme.ColorFor("status_bar_bg"));
            if (entry.focused) {
                title = std::move(title) | bold;
            } else {
                framed = std::move(framed) | Muted(deps_.theme);
            }
            framed = vbox({std::move(title), std::move(framed)}) |
                     borderStyled(deps_.theme.ColorFor(entry.focused ? "border_focused"
                                                                     : "border_unfocused"));
            rows.push_back(std::move(framed));
        }
    }

    rows.push_back(deps_.editor->Render());

    // Status bar. model/agent/permission_mode render from focused_details
    // below (the E5 metadata emission); empty/unknown renders as nothing,
    // per the format string's unknown-or-unfilled rule.
    std::map<std::string, std::string> values;
    std::map<std::string, ftxui::Color> emphasis;
    values["version"] = std::string(common::Version());
    // The focused tab's DEEP snapshot (see focused_details above) drives
    // the status bar's values. A SESSIONLESS tab renders too: it has a name
    // ("new session") and any staged /agent //model choices -- what it lacks
    // is only the server-side metadata, which fills in at EnsureSession.
    if (!focused.empty()) {
        values["session"] = !focused_details.display_name.empty()
                                ? focused_details.display_name
                                : std::string("new session");
        // A read-only transcript tab (OpenTranscriptTab) says so right in the
        // session segment -- the one place the user looks before typing. The
        // tab IS sessioned (its transcript renders); it just cannot be sent
        // to, which has_session's stream-derived false already encodes for
        // every consumer that acts on it.
        for (const auto& tab : tabs) {
            if (tab.tab_id == focused && tab.view_only) {
                values["session"] += "  (read-only)";
                break;
            }
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
            const int64_t window = focused_details.context_window_tokens;
            values["tokens"] = FormatTokenUsage(used, window);
            // Tinted as the window fills: attention from 80% (compaction is
            // near), error from 95%.
            if (window > 0) {
                const int64_t percent = used * 100 / window;
                if (percent >= 95) {
                    emphasis["tokens"] = deps_.theme.ColorFor("error");
                } else if (percent >= 80) {
                    emphasis["tokens"] = deps_.theme.ColorFor("attention");
                }
            }
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
        // A staged change reads as "→ gpt-5" -- switching to -- where it
        // used to be a bare "gpt-5~".
        if (!model_value.empty()) {
            values["model"] = (model_pending ? "→ " : "") + model_value;
        }
        if (!agent_value.empty()) {
            values["agent"] = (agent_pending ? "→ " : "") + agent_value;
        }
        {
            // The desired-state rule for the run mode: a shift+tab sent
            // but not yet confirmed renders the requested value with the
            // pending marker until the confirming metadata event lands
            // (the server applies the change immediately). See
            // PermissionModeSegmentValue.
            const std::string permission_value = PermissionModeSegmentValue(
                focused_details.permission_mode, focused_details.desired_permission_mode,
                !focused_details.session_id.empty());
            if (!permission_value.empty()) {
                values["permission_mode"] = permission_value;
                // bypass runs every tool unasked: the one mode that must never
                // look like the others. Colored by the DISPLAYED mode (marker
                // aside), so a pending bypass is already red -- it is about to
                // be true.
                const std::string& displayed_mode =
                    focused_details.desired_permission_mode.empty()
                        ? focused_details.permission_mode
                        : focused_details.desired_permission_mode;
                if (displayed_mode == "bypass") {
                    emphasis["permission_mode"] = deps_.theme.ColorFor("error");
                }
            }
        }
        // The paused segment (session.pause / SetPausedEvent): empty = not
        // paused = renders nothing, per the unknown-or-unfilled rule. Colored
        // at attention -- "the agent is suspended" is exactly the state the
        // user must notice before wondering why nothing is happening.
        if (focused_details.paused) {
            values["paused"] = "paused";
            emphasis["paused"] = deps_.theme.ColorFor("attention");
        }
        for (const auto& tab : tabs) {
            if (tab.tab_id != focused) continue;
            const auto activity = ActivityLabel(tab.state, tab.tools);
            if (!activity.empty()) values["activity"] = activity;
            switch (tab.state) {
                case SessionStateKind::Thinking:
                    emphasis["activity"] = deps_.theme.ColorFor("thinking");
                    break;
                case SessionStateKind::Working:
                    emphasis["activity"] = deps_.theme.ColorFor("working");
                    break;
                case SessionStateKind::Waiting:
                    emphasis["activity"] = deps_.theme.ColorFor("attention");
                    break;
                case SessionStateKind::Error:
                    emphasis["activity"] = deps_.theme.ColorFor("error");
                    break;
                case SessionStateKind::Done:
                case SessionStateKind::Idle:
                    break;
            }
            break;
        }
        // The interrupt press is never silent: from SendCancel until the
        // turn actually ends, the focused session says so -- in {activity},
        // which the default format always shows (it used to ride {session}).
        if (focused_details.cancel_requested) {
            values["activity"] = "cancelling…";
            emphasis["activity"] = deps_.theme.ColorFor("attention");
        }
        // The live-segment count cap dropped older segments of this turn:
        // the notice rides {activity} composed AFTER the state words above
        // (never overriding them -- WithHiddenRowsSuffix appends). In chrome
        // because it is about the session's live view, not a message; in the
        // status bar because a synthetic row in the stream read as model
        // output glued to whatever the page applies had not retired yet.
        if (focused_details.live_view_truncated) {
            values["activity"] = WithHiddenRowsSuffix(values["activity"]);
        }
    }
    // Registered segment providers fill their own ids. Render runs on the
    // render thread at ~20 Hz -- the provider contract (cheap, never
    // blocking) is stated on the interface; a THROW is contained (that
    // segment renders nothing for this frame), never fatal.
    for (const auto& segment : deps_.status_registry.segments()) {
        // Segments are SESSION-keyed (cost-view caches per focused session);
        // the frame's focused handle is a TAB id, so the snapshot's session
        // id goes out (empty = no tab / sessionless).
        const std::string id(segment->id());
        if (IsKnownStatusSegment(id)) continue;   // core ids are reserved (warned at startup)
        GuardPluginCall("segment:" + id, "status segment '" + id + "'", [&] {
            values[id] = segment->Render(focused_details.session_id);
        });
    }
    rows.push_back(RenderStatusBar(deps_.status_format, values, deps_.theme,
                                   ftxui::Terminal::Size().dimx, emphasis));

    return vbox(std::move(rows));
}

}  // namespace minimal::client
