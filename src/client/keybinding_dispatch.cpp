#include "minimal/client/keybinding_dispatch.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "minimal/client/key_event_parser.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

std::string NextRootGroup(const std::vector<AppState::TabSnapshot>& tabs,
                          const std::string& focused, bool forward) {
    std::vector<std::string> roots;
    for (const auto& tab : tabs) {
        if (!tab.is_subagent) roots.push_back(tab.tab_id);
    }
    if (roots.empty()) return "";

    std::size_t idx = 0;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i] == focused) idx = i;
    }
    return roots[(idx + (forward ? 1 : roots.size() - 1)) % roots.size()];
}

// --- Tab-bar filter (see the header for the contract) ------------------------

std::vector<AppState::TabSnapshot> FilterTabs(const std::vector<AppState::TabSnapshot>& tabs,
                                              TabFilter filter, const std::string& focused) {
    if (filter == TabFilter::All) return tabs;
    if (filter == TabFilter::Group) {
        // The focused tab's group only. An unknown focused handle falls back
        // to everything -- see the header.
        std::optional<int64_t> group;
        for (const auto& tab : tabs) {
            if (tab.tab_id == focused) {
                group = tab.root_ordinal;
                break;
            }
        }
        if (!group.has_value()) return tabs;
        std::vector<AppState::TabSnapshot> out;
        for (const auto& tab : tabs) {
            if (tab.root_ordinal == *group) out.push_back(tab);
        }
        return out;
    }
    // Agents: roots only.
    std::vector<AppState::TabSnapshot> out;
    for (const auto& tab : tabs) {
        if (!tab.is_subagent) out.push_back(tab);
    }
    return out;
}

std::string ResolveGroupRoot(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused) {
    std::optional<int64_t> group;
    for (const auto& tab : tabs) {
        if (tab.tab_id == focused) {
            group = tab.root_ordinal;
            break;
        }
    }
    if (!group.has_value()) return "";
    // The group's first non-subagent tab is its root (render order puts the
    // root before its children), so a focused ROOT resolves to itself here.
    for (const auto& tab : tabs) {
        if (tab.root_ordinal == *group && !tab.is_subagent) return tab.tab_id;
    }
    return "";
}

std::string NextVisibleTab(const std::vector<AppState::TabSnapshot>& tabs, TabFilter filter,
                           const std::string& focused, bool forward) {
    const auto visible = FilterTabs(tabs, filter, focused);
    if (visible.empty()) return "";
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        if (visible[i].tab_id == focused) {
            idx = i;
            found = true;
            break;
        }
    }
    // The old CycleFocus rule kept: nothing focused yet starts the cycle at
    // the first tab (forward) -- backward starts at the last.
    if (!found) return forward ? visible.front().tab_id : visible.back().tab_id;
    const std::size_t next =
        forward ? (idx + 1) % visible.size() : (idx + visible.size() - 1) % visible.size();
    return visible[next].tab_id;
}

TabFilterDecision ApplyTabFilterToggle(TabFilter current, TabFilter requested,
                                       const std::vector<AppState::TabSnapshot>& tabs,
                                       const std::string& focused) {
    TabFilterDecision d;
    d.filter = (requested == current) ? TabFilter::All : requested;
    if (d.filter == TabFilter::All) return d;   // unfiltering hides nothing

    // Only Agents can hide the focused tab (Group always contains it): land
    // focus on the group's root in the same press.
    if (d.filter == TabFilter::Agents) {
        bool focused_hidden = false;
        for (const auto& tab : tabs) {
            if (tab.tab_id == focused) {
                focused_hidden = tab.is_subagent;
                break;
            }
        }
        if (focused_hidden) d.focus_tab_id = ResolveGroupRoot(tabs, focused);
    }
    return d;
}

// --- Panel focus rules (see the header for the contract) ---------------------

v1::PermissionRunMode NextPermissionRunMode(const std::string& current) {
    if (current == "bypass") return v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    if (current == "allow-only") return v1::PERMISSION_RUN_MODE_NORMAL;
    return v1::PERMISSION_RUN_MODE_BYPASS;
}

void NotifyShown(const plugins::View& view) {
    try {
        view.OnShow();
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("plugin view '{}' OnShow threw: {}",
                                     std::string(view.spec().id), error.what());
    } catch (...) {
        common::DefaultLogger().warn("plugin view '{}' OnShow threw",
                                     std::string(view.spec().id));
    }
}

void FocusPanelOnOpen(std::vector<plugins::PluginViewEntry>& views,
                      plugins::PluginViewEntry& entry) {
    for (auto& other : views) other.focused = false;
    entry.focused = true;
}

void FocusPanelOnClose(std::vector<plugins::PluginViewEntry>& views,
                       plugins::PluginViewEntry& entry) {
    if (!entry.focused) return;
    entry.focused = false;
    for (auto& other : views) {
        if (&other != &entry &&
            other.view->spec().placement == plugins::ViewPlacement::Panel && other.visible) {
            other.focused = true;
            break;
        }
    }
}

void ShowPluginView(std::vector<plugins::PluginViewEntry>& views,
                    plugins::PluginViewEntry& entry,
                    ftxui::Component& modal_slot, bool& show_modal) {
    if (entry.view->spec().placement == plugins::ViewPlacement::Modal) {
        for (auto& shown : views) {
            if (&shown != &entry &&
                shown.view->spec().placement == plugins::ViewPlacement::Modal) {
                shown.visible = false;
            }
        }
        if (entry.component == nullptr) {
            common::DefaultLogger().warn("plugin view '{}' has no component; modal not opened",
                                         entry.surface_id);
            entry.visible = false;
            modal_slot = nullptr;
            show_modal = false;
            return;
        }
        entry.visible = true;
        modal_slot = entry.component;
        show_modal = true;
    } else {
        const bool was_visible = entry.visible;
        entry.visible = true;
        if (!was_visible) FocusPanelOnOpen(views, entry);
    }
    NotifyShown(*entry.view);
}

bool ApplyViewRequest(std::vector<plugins::PluginViewEntry>& views,
                      const plugins::ViewRequest& request, ftxui::Component& modal_slot,
                      bool& show_modal) {
    using Op = plugins::ViewRequest::Op;
    for (auto& entry : views) {
        // A plugin's own request names its view's canonical id; anything else
        // (a server view hint) names the surface the user sees.
        const bool addressed = request.plugin.empty()
                                   ? entry.surface_id == request.view_id
                                   : entry.plugin == request.plugin &&
                                         entry.view->spec().id == request.view_id;
        if (!addressed) continue;
        const bool is_panel = entry.view->spec().placement == plugins::ViewPlacement::Panel;
        const bool show = request.op == Op::kShow ||
                          (request.op == Op::kSetPanelVisible && request.visible);
        if (request.op == Op::kSetPanelVisible && !is_panel) return false;
        if (request.op == Op::kFocusPanel) {
            if (!is_panel || !entry.visible) return false;
            FocusPanelOnOpen(views, entry);
            return true;
        }
        if (show) {
            ShowPluginView(views, entry, modal_slot, show_modal);
            return true;
        }
        // Close / hide.
        if (!entry.visible) return false;
        entry.visible = false;
        if (is_panel) {
            FocusPanelOnClose(views, entry);
        } else {
            show_modal = false;
        }
        return true;
    }
    return false;
}

void CyclePanelFocus(std::vector<plugins::PluginViewEntry>& views, bool forward) {
    std::vector<plugins::PluginViewEntry*> visible;
    for (auto& entry : views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible) {
            visible.push_back(&entry);
        }
    }
    if (visible.empty()) return;

    std::size_t current = visible.size();  // none focused
    for (std::size_t i = 0; i < visible.size(); ++i) {
        if (visible[i]->focused) current = i;
    }
    std::size_t next = 0;
    if (current != visible.size() && visible.size() > 1) {
        next = (current + (forward ? 1 : visible.size() - 1)) % visible.size();
    }
    // EVERY flag clears, visible or not: focus is "at most one, period" -- a
    // panel that just became invisible must not carry a stale flag around
    // (the frame's chrome and the dispatch routing both read the flag).
    for (auto& entry : views) entry.focused = false;
    visible[next]->focused = true;
}

bool ClosePluginModal(std::vector<plugins::PluginViewEntry>& views,
                      std::shared_ptr<ftxui::Component>& modal_child_slot, bool& show_modal) {
    for (auto& entry : views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Modal && entry.visible) {
            // Mirrors HandleGlobalKey's own toggle-off exactly (entry.visible,
            // the slot reassignment, show_modal) -- see the header for why a
            // second, host-level closer is needed at all: FTXUI's Modal()
            // gives the shown modal exclusive input, so the toggle keybinding
            // below (routed through HandleGlobalKey, i.e. "main" in Modal()'s
            // terms) can never run again to close it.
            entry.visible = false;
            *modal_child_slot = entry.component;
            show_modal = false;
            return true;
        }
    }
    return false;
}

bool TabBusyForInterrupt(const bool turn_in_flight, const SessionStateKind state) {
    switch (state) {
        case SessionStateKind::Thinking:
        case SessionStateKind::Working:
        case SessionStateKind::Waiting:
            return true;
        default:
            // Idle (and Done/Error): only an armed turn-in-flight makes this
            // tab interruptible. Done/Error with the flag standing is the
            // ensure-session window -- a submit the server has not acked
            // with any wire event yet.
            return turn_in_flight;
    }
}

std::string NextAttentionTab(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].tab_id == focused) {
            start = i;
            break;
        }
    }
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const std::size_t idx = (start + 1 + i) % tabs.size();   // +1: skip current
        if (tabs[idx].attention_glyph == '!') return tabs[idx].tab_id;
    }
    return "";
}

bool AnyWorkInFlight(const std::vector<AppState::TabSnapshot>& tabs,
                    const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        for (const auto& tab : tabs) {
            if (tab.tab_id != id) continue;
            // Same probe rule as TabBusyForInterrupt: a turn silent between
            // message stops is still live, and closing its tab must confirm
            // rather than terminate it silently.
            switch (tab.state) {
                case SessionStateKind::Thinking:
                case SessionStateKind::Working:
                case SessionStateKind::Waiting:
                    return true;
                default:
                    if (tab.turn_in_flight) return true;
                    break;
            }
            break;
        }
    }
    return false;
}

ResolvedKeymap ResolvedKeymap::Build(const Keybindings& keys) {
    ResolvedKeymap out;
    for (const auto& [action, key] : keys.all()) {
        auto events = ParseKeySpec(key);
        if (events.empty()) {
            out.unmapped_.emplace_back(action, key);
            continue;
        }
        out.table_.emplace(action, std::move(events));
    }
    return out;
}

bool ResolvedKeymap::Matches(std::string_view action, const ftxui::Event& event) const {
    // Transparent hash: no std::string materialized per probe.
    const auto it = table_.find(action);
    if (it == table_.end()) return false;   // never bound, or bound to an unparseable key
    for (const auto& candidate : it->second) {
        if (event == candidate) return true;
    }
    return false;
}

std::vector<std::string> TabsToClose(const std::vector<AppState::TabSnapshot>& tabs,
                                     const std::string& sid) {
    std::optional<int64_t> group;
    bool is_root = false;
    for (const auto& tab : tabs) {
        if (tab.tab_id == sid) {
            group = tab.root_ordinal;
            is_root = !tab.is_subagent;
        }
    }
    std::vector<std::string> staged;
    for (const auto& tab : tabs) {
        if (!is_root && tab.tab_id != sid) continue;
        if (is_root && group.has_value() && tab.root_ordinal != *group) continue;
        staged.push_back(tab.tab_id);
    }
    return staged;
}

bool HandleGlobalKey(KeyContext& ctx, const ftxui::Event& event) {
    using ftxui::Event;
    using ftxui::Mouse;

    // The focused prompt editor's own keys come first (PromptEditor::Claims):
    // an open @/-suggestion list's navigation, accept and dismiss keys --
    // ahead of the busy-turn Escape and a focused panel's Tab/arrows/Return,
    // which would otherwise eat them -- and a pasted macOS Option-glyph byte,
    // byte-identical to the alt+key it would otherwise fire.
    if (ctx.editor_claims && ctx.editor_claims(event)) return false;

    const std::string& sid = ctx.focused;

    // The ACTIVE tab filter (TabFilter): every tab-keyed navigation below
    // acts on the VISIBLE set. A filter that hid tabs from the bar but not
    // from alt+down/up would cycle focus through tabs the user cannot see;
    // tab.goto.N's index contract is "the number the bar prints", which is
    // the filtered list's numbering. jump.attention is the one exception
    // (its own branch, below): it scans everything, deliberately.
    const TabFilter filter = ctx.app.ActiveTabFilter();

    if (ctx.keymap.Matches("tab.next", event)) {
        const auto target = NextVisibleTab(ctx.app.GetAllTabs(), filter, sid, /*forward=*/true);
        if (!target.empty()) ctx.app.SetFocusedTab(target);
        return true;
    }
    if (ctx.keymap.Matches("tab.previous", event)) {
        const auto target = NextVisibleTab(ctx.app.GetAllTabs(), filter, sid, /*forward=*/false);
        if (!target.empty()) ctx.app.SetFocusedTab(target);
        return true;
    }
    // tab.filter_group / tab.filter_agents: two toggles over the ONE shared
    // filter state (TabFilter). The active filter pressed again returns to
    // All; the other replaces it. When the new filter would hide the focused
    // tab (Agents over a subagent), ApplyTabFilterDecision moves focus to
    // the group's root in the same press, so focus never sits on a tab the
    // bar does not show. Unfiltering (back to All) touches no focus.
    if (ctx.keymap.Matches("tab.filter_group", event) ||
        ctx.keymap.Matches("tab.filter_agents", event)) {
        const auto tabs = ctx.app.GetAllTabs();
        const auto decision = ApplyTabFilterToggle(
            ctx.app.ActiveTabFilter(),
            ctx.keymap.Matches("tab.filter_group", event) ? TabFilter::Group : TabFilter::Agents,
            tabs, sid);
        if (!decision.focus_tab_id.empty()) ctx.app.SetFocusedTab(decision.focus_tab_id);
        ctx.app.SetTabFilter(decision.filter);
        return true;
    }
    // tab.goto.1..9: straight to the Nth tab, in tab-bar order (the number the
    // bar prints before each name) -- the FILTERED list when a tab filter is
    // active, per the ActiveTabFilter note above. A number past the last tab
    // is consumed and ignored, like tab.next at an edge. The names are built
    // once, not per event.
    static const std::array<std::string, 9> kGotoActions = [] {
        std::array<std::string, 9> names;
        for (std::size_t i = 0; i < names.size(); ++i) {
            names[i] = "tab.goto." + std::to_string(i + 1);
        }
        return names;
    }();
    for (std::size_t i = 0; i < kGotoActions.size(); ++i) {
        if (!ctx.keymap.Matches(kGotoActions[i], event)) continue;
        const auto visible = FilterTabs(ctx.app.GetAllTabs(), filter, sid);
        if (i < visible.size()) ctx.app.SetFocusedTab(visible[i].tab_id);
        return true;
    }
    const bool group_next = ctx.keymap.Matches("group.next", event);
    const bool group_prev = ctx.keymap.Matches("group.previous", event);
    if (group_next || group_prev) {
        // Between root groups: focus the next (or previous) ROOT tab. The
        // index math is NextRootGroup, above -- this is the same shape of
        // "find my index among a filtered subset" bug that jump.attention
        // had (below). Runs over the VISIBLE set: under the Agents filter
        // that is the roots themselves; under Group the cycle stays inside
        // the visible group.
        const auto visible = FilterTabs(ctx.app.GetAllTabs(), filter, sid);
        const auto target = NextRootGroup(visible, sid, group_next);
        if (!target.empty()) ctx.app.SetFocusedTab(target);
        return true;
    }
    if (ctx.keymap.Matches("jump.attention", event)) {
        // Crosses groups; wraps. Only tabs carrying the ! glyph qualify.
        // Scans starting just AFTER the focused tab -- not a fixed index,
        // which ignored which tab was actually focused and could "jump" to
        // the focused tab itself, a visible no-op exactly when Waiting is
        // the reason someone pressed the key. See NextAttentionTab, which
        // this bug is now a unit test of.
        //
        // Scans ALL tabs, filtered or not: this is the cross-group RESCUE
        // verb, and a '!' hidden by the tab filter is exactly when it is
        // needed most (the tab bar's filter badge shows the hidden count).
        // When the target IS filtered out, the filter clears first so the
        // jump is actually visible -- one keypress always lands on the
        // prompt, never on nothing.
        const auto tabs = ctx.app.GetAllTabs();
        const auto target = NextAttentionTab(tabs, sid);
        if (!target.empty()) {
            const auto visible = FilterTabs(tabs, filter, sid);
            const bool target_visible = std::any_of(visible.begin(), visible.end(),
                                                    [&](const auto& tab) {
                                                        return tab.tab_id == target;
                                                    });
            if (!target_visible) ctx.app.SetTabFilter(TabFilter::All);
            ctx.app.SetFocusedTab(target);
        }
        return true;
    }
    // INTERRUPT -- the interactive loop's missing verb. Escape and ctrl+c on
    // a busy tab send CancelTurn; the server ends the turn with
    // STOP_REASON_CANCELLED and answers any prompt it was parked on with a
    // denial (pending.CancelAll), so a wedged prompt is interruptible too.
    // Waiting counts as busy. Modals sit OUTERMOST in the stack, so while
    // one is up it consumes these keys first (permission: esc = deny;
    // confirmation: esc = a cancel option when offered) -- this branch only
    // fires with no modal shown. The busy probe runs only when one of the
    // two keys actually arrived, never per keystroke.
    const bool cancel_pressed = event == Event::CtrlC || ctx.keymap.Matches("turn.cancel", event);
    if (cancel_pressed) {
        bool busy = false;
        if (!sid.empty()) {
            for (const auto& tab : ctx.app.GetAllTabs()) {
                if (tab.tab_id != sid) continue;
                // The probe outranks the display state: see
                // TabBusyForInterrupt. The old inline switch trusted the
                // state alone, so a turn silent between message stops read
                // as idle -- ESC did nothing and ctrl+c quit the app.
                busy = TabBusyForInterrupt(tab.turn_in_flight, tab.state);
                break;
            }
        }
        if (busy) {
            // Busy implies sessioned (a sessionless tab has no turn to be in
            // flight), so the SESSION-keyed cancel takes the focused tab's
            // session id, not its tab handle. EXCEPT the ensure-session
            // window: a submit on a sessionless tab arms turn_in_flight while
            // the single-flight create is still on the async RPC worker, so
            // busy with no session id is REACHABLE -- surface it on the tab's
            // notice line instead of silently eating the press (the old
            // shape: consumed, no hint, no effect, no explanation).
            const std::string session = ctx.app.FocusedSessionId();
            if (!session.empty()) {
                ctx.app.SendCancel(session);
            } else {
                ctx.app.TabNotice(sid, "cannot cancel: this tab has no attached session yet");
            }
            return true;
        }
        // Idle ctrl+c means quit: the universal exit reflex, otherwise DEAD
        // (raw mode -- no SIGINT ever reaches this process). Idle Escape used
        // to fall through to the editor, which has no use for it either --
        // dismissing a lingering notice line (a stale "/theme x" confirmation,
        // an old /cost table) gives it a job, matching the same "Escape
        // closes the innermost transient thing" ladder the modals already
        // follow. Only consumed when there WAS a notice to drop: with
        // nothing to do, the key still falls through unchanged.
        if (event == Event::CtrlC) {
            ctx.request_quit();
            return true;
        }
        if (!sid.empty() && ctx.app.ClearTabNotice(sid)) return true;
    }
    if (ctx.keymap.Matches("session.picker", event)) {
        // Always OPENS, never toggles closed -- see the KeyContext field
        // comment: the modal has exclusive input while shown, so this key
        // is unreachable again until it closes some other way, and the old
        // `show_picker = !show_picker` toggle's close half was dead code.
        ctx.open_picker();
        return true;
    }
    if (ctx.keymap.Matches("agent.picker", event)) {
        ctx.open_agent_picker();
        return true;
    }
    if (ctx.keymap.Matches("model.picker", event)) {
        ctx.open_model_picker();
        return true;
    }
    if (ctx.keymap.Matches("permission.cycle", event)) {
        // shift+tab cycles normal -> bypass -> allow-only -> normal as a
        // DIRECT wire event (SendSetRunMode) -- no command, no confirmation
        // modal: the keybinding IS the confirmation. The server applies the
        // change immediately (an atomic flip, like SetPaused), so a mid-turn
        // press gates the running turn's very next tool call; the metadata
        // event it publishes is what the status bar's {permission_mode}
        // segment refreshes from. An unknown/absent mode string counts as
        // normal (the server default a fresh session starts on).
        if (!sid.empty()) {
            // GetDetails is TAB-keyed (the focused handle may be a
            // sessionless tab); the mode switch itself is a session-keyed
            // wire event, so a sessionless tab simply has nothing to cycle.
            const auto details = ctx.app.GetDetails(sid);
            // The cycle advances from the QUEUED mode when one is pending:
            // the event is now applied server-side within microseconds, but
            // the round-trip metadata event still lands on a later read, and
            // cycling from the not-yet-refreshed applied value would make two
            // rapid presses repeat the first transition instead of walking
            // normal -> bypass -> allow-only.
            const std::string& current = details.desired_permission_mode.empty()
                                             ? details.permission_mode
                                             : details.desired_permission_mode;
            const std::string session = ctx.app.FocusedSessionId();
            if (session.empty()) return true;
            ctx.app.SendSetRunMode(session, NextPermissionRunMode(current));
        }
        return true;
    }
    if (ctx.keymap.Matches("session.pause", event)) {
        // alt+p toggles the session's pause state as a DIRECT wire event
        // (SendSetPaused) -- no command, no confirmation modal: the
        // keybinding IS the confirmation, the same contract shift+tab's
        // run-mode cycle has. The server applies this on its reader thread
        // (an atomic flip), so a mid-turn press takes effect at the next
        // tool-call return of the running turn rather than at a turn
        // boundary -- exactly the contract the run-mode switch now shares.
        // A sessionless tab has nothing to pause.
        if (!sid.empty()) {
            const std::string session = ctx.app.FocusedSessionId();
            if (session.empty()) return true;
            const bool current = ctx.app.GetDetails(sid).paused;
            ctx.app.SendSetPaused(session, !current);
        }
        return true;
    }
    if (ctx.keymap.Matches("chat.load_older", event)) {
        // Session-keyed fetch: a sessionless tab has no scrollback to page.
        if (const std::string session = ctx.app.FocusedSessionId(); !session.empty()) {
            ctx.app.RequestScrollback(session, /*newest=*/false);
        }
        return true;
    }
    if (event.is_mouse()) {
        // Event::mouse() isn't const-qualified upstream, unlike is_mouse();
        // a local mutable copy is cheap and this handler never mutates it.
        Event mouse_event = event;
        const auto& mouse = mouse_event.mouse();
        // Left-press on a tab-bar cell focuses that tab.
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed && ctx.tab_at) {
            if (const std::string tab = ctx.tab_at(mouse.x, mouse.y); !tab.empty()) {
                ctx.app.SetFocusedTab(tab);
                return true;
            }
        }
        // The chat's own: the wheel over it, a click on a collapsible or a
        // queued row. Anything else falls through.
        if (!sid.empty() && ctx.chat_mouse && ctx.chat_mouse(sid, event)) return true;
    }
    for (const auto& action : ctx.chat_actions) {
        if (!ctx.keymap.Matches(action, event)) continue;
        if (!sid.empty() && ctx.chat_action) ctx.chat_action(sid, action);
        return true;
    }
    if (ctx.keymap.Matches("tab.close", event)) {
        if (!sid.empty()) {
            // Closing a ROOT tab takes its whole group: TabsToClose stages
            // the group in render order -- root FIRST, children after (the
            // old "children first" claim inverted the actual order). ONE
            // confirmation if any of it is in flight; app.cpp's confirm modal
            // owns staging that (PendingClose, show_quit_confirm) and now
            // asks close-vs-detach -- "detach" ends the streams with an
            // explicit EndSession{DETACH} so the sessions keep running
            // (/sessions reattaches), "close anyway" TERMINATEs. Idle groups
            // close straight through: the bare stream drop reads as a DETACH
            // server-side either way. This function stays ignorant of both
            // types.
            const auto tabs = ctx.app.GetAllTabs();
            auto ids = TabsToClose(tabs, sid);
            if (AnyWorkInFlight(tabs, ids)) {
                ctx.confirm_close(std::move(ids));
            } else {
                for (const auto& id : ids) ctx.close_tab(id);
            }
        }
        return true;
    }
    if (ctx.keymap.Matches("app.quit", event)) {
        ctx.request_quit();
        return true;
    }

    // Panel focus cycling among VISIBLE plugin panels (see CyclePanelFocus
    // and the header's focus-rule contract). With no panel visible the key
    // falls through: the action has nothing to act on, and a user who bound
    // it to an editing key (the old ctrl+e default) keeps that key working
    // in the prompt whenever no panel is up.
    if (ctx.keymap.Matches("panel.focus.next", event) ||
        ctx.keymap.Matches("panel.focus.prev", event)) {
        const bool any_panel_visible = std::any_of(
            ctx.plugin_views.begin(), ctx.plugin_views.end(), [](const auto& entry) {
                return entry.visible &&
                       entry.view->spec().placement == plugins::ViewPlacement::Panel;
            });
        if (any_panel_visible) {
            CyclePanelFocus(ctx.plugin_views, ctx.keymap.Matches("panel.focus.next", event));
            return true;
        }
    }

    // Plugin views, after every core action: the FOCUSED panel gets the event
    // (its component consumes what it wants); any view id bound in
    // keybindings.json toggles its visibility. The ids were registered as
    // runtime defaults (EnsureDefault) BEFORE the keymap was built, so the
    // keymap already knows them -- a user's file that already names them
    // still wins (EnsureDefault only fills what's unbound).
    bool any_focused = false;
    for (const auto& entry : ctx.plugin_views) {
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel &&
            entry.visible && entry.focused) {
            any_focused = true;
        }
    }
    for (auto& entry : ctx.plugin_views) {
        if (entry.view->spec().placement != plugins::ViewPlacement::Panel || !entry.visible) {
            continue;
        }
        // Focus routing: when some visible panel holds focus, only it
        // receives events. With no focus holder -- the degenerate state a
        // mid-session load could produce -- every visible panel is offered
        // the event in order, the pre-focus behavior.
        if (any_focused && !entry.focused) {
            continue;
        }
        // OnEvent is plugin code on the input path: a throw here propagated
        // out of the event loop and std::terminate'd the TUI. Contained, the
        // event is treated as not consumed and the rest of dispatch proceeds.
        bool consumed = false;
        try {
            consumed = entry.component->OnEvent(event);
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("plugin view '{}' threw on an event: {}",
                                         entry.surface_id, error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "plugin view '{}' threw a non-standard exception on an event", entry.surface_id);
        }
        if (consumed) {
            return true;
        }
    }
    for (auto& entry : ctx.plugin_views) {
        if (ctx.keymap.Matches(entry.surface_id, event)) {
            if (!entry.visible) {
                ShowPluginView(ctx.plugin_views, entry, *ctx.plugin_modal_child_slot,
                               ctx.show_plugin_modal);
                return true;
            }
            // Toggle off. Focus follows -- see the header's focus-rule
            // contract.
            entry.visible = false;
            if (entry.view->spec().placement == plugins::ViewPlacement::Panel) {
                FocusPanelOnClose(ctx.plugin_views, entry);
            } else {
                ctx.show_plugin_modal = false;
            }
            return true;
        }
    }
    return false;
}

}  // namespace minimal::client
