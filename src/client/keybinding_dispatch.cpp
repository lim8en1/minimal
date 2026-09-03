#include "minimal/client/keybinding_dispatch.hpp"

#include "minimal/client/key_event_parser.hpp"

namespace minimal::client {

std::string NextRootGroup(const std::vector<AppState::TabSnapshot>& tabs,
                          const std::string& focused, bool forward) {
    std::vector<std::string> roots;
    for (const auto& tab : tabs) {
        if (!tab.is_subagent) roots.push_back(tab.session_id);
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
        if (tabs[i].session_id == focused) {
            start = i;
            break;
        }
    }
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const std::size_t idx = (start + 1 + i) % tabs.size();   // +1: skip current
        if (tabs[idx].attention_glyph == '!') return tabs[idx].session_id;
    }
    return "";
}

bool AnyWorkInFlight(const std::vector<AppState::TabSnapshot>& tabs,
                    const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        for (const auto& tab : tabs) {
            if (tab.session_id != id) continue;
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
    const auto it = table_.find(std::string(action));
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
        if (tab.session_id == sid) {
            group = tab.root_ordinal;
            is_root = !tab.is_subagent;
        }
    }
    std::vector<std::string> staged;
    for (const auto& tab : tabs) {
        if (!is_root && tab.session_id != sid) continue;
        if (is_root && group.has_value() && tab.root_ordinal != *group) continue;
        staged.push_back(tab.session_id);
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
                if (tab.session_id != sid) continue;
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
            ctx.app.SendCancel(sid);
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
    if (ctx.keymap.Matches("permission.cycle", event)) {
        // The interactive loop for /permission-mode: shift+tab cycles
        // normal -> bypass -> allow-only -> normal, then re-sends the result
        // through the EXACT path a typed command takes (SendCommand) --
        // server-side validation, the bypass confirmation modal (widening
        // permissions is exactly the thing worth asking about) and the
        // status-bar segment update all come from the existing machinery,
        // not from this handler. An unknown/absent mode string counts as
        // normal (the server default a fresh session starts on).
        if (!sid.empty()) {
            const std::string current = ctx.app.GetDetails(sid).permission_mode;
            std::string next;
            if (current == "bypass") {
                next = "allow-only";
            } else if (current == "allow-only") {
                next = "normal";
            } else {
                next = "bypass";   // "normal", empty, or unrecognized
            }
            ctx.app.SendCommand(sid, "/permission-mode " + next);
        }
        return true;
    }
    if (ctx.keymap.Matches("panel.toggle", event)) {
        ctx.show_panel = !ctx.show_panel;
        return true;
    }
    if (ctx.keymap.Matches("chat.load_older", event)) {
        if (!sid.empty()) ctx.app.RequestScrollback(sid, /*newest=*/false);
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
    if (ctx.keymap.Matches("tab.close", event)) {
        if (!sid.empty()) {
            // Closing a ROOT tab terminates its group: stage children first,
            // the root last (TabsToClose, above). ONE confirmation if any of
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
        if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible &&
            entry.component->OnEvent(event)) {
            return true;
        }
    }
    for (auto& entry : ctx.plugin_views) {
        if (ctx.keymap.Matches(entry.view->spec().id, event)) {
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
