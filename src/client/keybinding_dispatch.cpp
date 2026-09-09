#include "minimal/client/keybinding_dispatch.hpp"

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
            switch (tab.state) {
                case SessionStateKind::Thinking:
                case SessionStateKind::Working:
                case SessionStateKind::Waiting:
                    return true;
                default:
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

    const std::string& sid = ctx.focused;

    if (ctx.keymap.Matches("tab.next", event)) {
        ctx.app.CycleFocusForward();
        return true;
    }
    if (ctx.keymap.Matches("tab.previous", event)) {
        ctx.app.CycleFocusBackward();
        return true;
    }
    const bool group_next = ctx.keymap.Matches("group.next", event);
    const bool group_prev = ctx.keymap.Matches("group.previous", event);
    if (group_next || group_prev) {
        // Between root groups: focus the next (or previous) ROOT tab. The
        // index math is NextRootGroup, above -- this is the same shape of
        // "find my index among a filtered subset" bug that jump.attention
        // had (below).
        const auto tabs = ctx.app.GetAllTabs();
        const auto target = NextRootGroup(tabs, sid, group_next);
        if (!target.empty()) ctx.app.SetFocusedTab(target);
        return true;
    }
    if (ctx.keymap.Matches("jump.attention", event)) {
        // Crosses groups; wraps. Only Waiting tabs qualify (the ! glyph).
        // Scans starting just AFTER the focused tab -- not a fixed index,
        // which ignored which tab was actually focused and could "jump" to
        // the focused tab itself, a visible no-op exactly when Waiting is
        // the reason someone pressed the key. See NextAttentionTab, which
        // this bug is now a unit test of.
        const auto target = NextAttentionTab(ctx.app.GetAllTabs(), sid);
        if (!target.empty()) ctx.app.SetFocusedTab(target);
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
                switch (tab.state) {
                    case SessionStateKind::Thinking:
                    case SessionStateKind::Working:
                    case SessionStateKind::Waiting:
                        busy = true;
                        break;
                    default:
                        break;
                }
                break;
            }
        }
        if (busy) {
            // Busy implies sessioned (a sessionless tab has no turn to be in
            // flight), so the SESSION-keyed cancel takes the focused tab's
            // session id, not its tab handle.
            const std::string session = ctx.app.FocusedSessionId();
            if (!session.empty()) ctx.app.SendCancel(session);
            return true;
        }
        // Idle ctrl+c means quit: the universal exit reflex, otherwise DEAD
        // (raw mode -- no SIGINT ever reaches this process). Idle Escape
        // falls through to the editor, which has no use for it.
        if (event == Event::CtrlC) {
            ctx.request_quit();
            return true;
        }
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
    if (ctx.keymap.Matches("help.overlay", event)) {
        ctx.open_keymap_help();
        return true;
    }
    if (ctx.keymap.Matches("permission.cycle", event)) {
        // shift+tab cycles normal -> bypass -> allow-only -> normal as a
        // DIRECT wire event (SendSetRunMode) -- no command, no confirmation
        // modal: the keybinding IS the confirmation. The server queues the
        // change for its worker (applies at a turn boundary) and announces
        // it back via SessionMetadataEvent, which is what the status bar's
        // {permission_mode} segment refreshes from. An unknown/absent mode
        // string counts as normal (the server default a fresh session
        // starts on).
        if (!sid.empty()) {
            // GetDetails is TAB-keyed (the focused handle may be a
            // sessionless tab); the mode switch itself is a session-keyed
            // wire event, so a sessionless tab simply has nothing to cycle.
            const std::string& current = ctx.app.GetDetails(sid).permission_mode;
            const std::string session = ctx.app.FocusedSessionId();
            if (session.empty()) return true;
            v1::PermissionRunMode next = v1::PERMISSION_RUN_MODE_BYPASS;
            if (current == "bypass") {
                next = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
            } else if (current == "allow-only") {
                next = v1::PERMISSION_RUN_MODE_NORMAL;
            }
            ctx.app.SendSetRunMode(session, next);
        }
        return true;
    }
    if (ctx.keymap.Matches("panel.toggle", event)) {
        ctx.show_panel = !ctx.show_panel;
        return true;
    }
    if (ctx.keymap.Matches("chat.load_older", event)) {
        // Session-keyed fetch: a sessionless tab has no scrollback to page.
        if (const std::string session = ctx.app.FocusedSessionId(); !session.empty()) {
            ctx.app.RequestScrollback(session, /*newest=*/false);
        }
        return true;
    }
    // In-view scrollback: moves which already-rendered row is focused (see
    // ChatView::ScrollBy), distinct from chat.load_older above, which
    // fetches an OLDER PAGE from the server. ctx.scroll_chat is a no-op if
    // the tab has no ChatView yet (the very first frame of a freshly opened
    // tab, before session_view's render lambda lazily builds one).
    if (event.is_mouse()) {
        // Event::mouse() isn't const-qualified upstream, unlike is_mouse();
        // a local mutable copy is cheap and this handler never mutates it.
        Event mouse_event = event;
        const auto& mouse = mouse_event.mouse();
        if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
            if (!sid.empty()) {
                // WheelUp = scroll toward older (positive), matching
                // conventional scroll-up-reveals-earlier-content.
                ctx.scroll_chat(sid, mouse.button == Mouse::WheelUp ? 3 : -3);
            }
            return true;
        }
        // Left-press collapse toggle: only consumed when the click actually
        // landed on a collapsible row (thinking / tool activity / user
        // attachment). Everything else -- a miss, empty space, the editor --
        // falls through to the components below, so normal clicking behavior
        // (text selection, the editor's own mouse handling) is untouched.
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
            if (!sid.empty() && ctx.toggle_collapse_at &&
                ctx.toggle_collapse_at(sid, mouse.y)) {
                return true;
            }
        }
    }
    if (ctx.keymap.Matches("chat.scroll_up", event)) {
        if (!sid.empty()) ctx.scroll_chat(sid, 1);   // toward older
        return true;
    }
    if (ctx.keymap.Matches("chat.scroll_down", event)) {
        if (!sid.empty()) ctx.scroll_chat(sid, -1);   // toward newest
        return true;
    }
    if (ctx.keymap.Matches("chat.scroll_bottom", event)) {
        if (!sid.empty()) ctx.scroll_chat_to_bottom(sid);
        return true;
    }
    if (ctx.keymap.Matches("chat.row_up", event)) {
        if (!sid.empty()) ctx.move_row_focus(sid, 1);   // toward older
        return true;
    }
    if (ctx.keymap.Matches("chat.row_down", event)) {
        if (!sid.empty()) ctx.move_row_focus(sid, -1);   // toward newest
        return true;
    }
    if (ctx.keymap.Matches("chat.toggle_collapse", event)) {
        if (!sid.empty()) ctx.toggle_collapse_focused(sid);
        return true;
    }
    if (ctx.keymap.Matches("chat.toggle_collapse_all", event)) {
        if (!sid.empty()) ctx.toggle_collapse_all(sid);
        return true;
    }
    if (ctx.keymap.Matches("chat.toggle_timestamps", event)) {
        if (!sid.empty()) ctx.toggle_timestamps(sid);
        return true;
    }
    if (ctx.keymap.Matches("tab.close", event)) {
        if (!sid.empty()) {
            // Closing a ROOT tab terminates its group: TabsToClose stages the
            // group in render order -- root FIRST, children after (the old
            // "children first" claim inverted the actual order; either works,
            // since the root's close terminates the children's sessions and
            // their closes degrade to no-ops). ONE confirmation if any of
            // it is in flight; app.cpp's confirm_close owns staging that
            // (PendingClose, show_quit_confirm) -- this function stays
            // ignorant of both types.
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

    // Plugin views, after every core action: a visible PANEL gets the event
    // first (its component consumes what it wants); any view id bound in
    // keybindings.json toggles its visibility. The ids were registered as
    // runtime defaults (EnsureDefault) BEFORE the keymap was built, so the
    // keymap already knows them -- a user's file that already names them
    // still wins (EnsureDefault only fills what's unbound).
    for (auto& entry : ctx.plugin_views) {
        if (entry.view->spec().placement != plugins::ViewPlacement::Panel || !entry.visible) {
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
                                         std::string(entry.view->spec().id), error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "plugin view '{}' threw a non-standard exception on an event",
                std::string(entry.view->spec().id));
        }
        if (consumed) {
            return true;
        }
    }
    for (auto& entry : ctx.plugin_views) {
        if (ctx.keymap.Matches(entry.view->spec().id, event)) {
            // There is ONE modal slot: opening modal B while modal A is
            // shown must retire A's `visible` flag along with replacing the
            // slot's child. Left set, A counted as shown while it was not
            // displayed, so A's own keypress first "toggled" it off -- one
            // dead keypress before A could actually open.
            if (entry.view->spec().placement == plugins::ViewPlacement::Modal) {
                for (auto& shown : ctx.plugin_views) {
                    if (&shown != &entry &&
                        shown.view->spec().placement == plugins::ViewPlacement::Modal) {
                        shown.visible = false;
                    }
                }
            }
            entry.visible = !entry.visible;
            if (entry.view->spec().placement == plugins::ViewPlacement::Modal) {
                *ctx.plugin_modal_child_slot = entry.component;
                ctx.show_plugin_modal = entry.visible;
            }
            return true;
        }
    }
    return false;
}

}  // namespace minimal::client
