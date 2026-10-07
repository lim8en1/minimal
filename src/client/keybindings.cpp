#include "minimal/client/keybindings.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/client/key_event_parser.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {

// The shipped defaults (bundled/keybindings.json seeds the same content). Every
// name here is a core action; the file may add any others.
const std::unordered_map<std::string, std::string>& DefaultBindings() {
    static const std::unordered_map<std::string, std::string> kDefaults{
        {"tab.next", "alt+down"},
        {"tab.previous", "alt+up"},
        // Straight to the Nth tab -- the number the tab bar prints before
        // each name. alt+digit is ESC+digit (or the macOS Option glyph), a
        // distinct encoding from the plain digit the editor types.
        {"tab.goto.1", "alt+1"},
        {"tab.goto.2", "alt+2"},
        {"tab.goto.3", "alt+3"},
        {"tab.goto.4", "alt+4"},
        {"tab.goto.5", "alt+5"},
        {"tab.goto.6", "alt+6"},
        {"tab.goto.7", "alt+7"},
        {"tab.goto.8", "alt+8"},
        {"tab.goto.9", "alt+9"},
        {"group.previous", "alt+left"},
        {"group.next", "alt+right"},
        // Tab-bar filters (keybinding_dispatch.hpp's FilterTabs): alt+f
        // shows only the focused tab's group, alt+o only the agent (root)
        // tabs. The active filter pressed again returns to all. Plain
        // alt+letters: single-candidate encodings, unclaimed by any other
        // core action.
        {"tab.filter_group", "alt+f"},
        {"tab.filter_agents", "alt+o"},
        {"jump.attention", "alt+a"},
        {"session.picker", "ctrl+o"},
        {"agent.picker", "alt+g"},
        {"model.picker", "alt+m"},
        {"permission.cycle", "shift+tab"},
        {"session.pause", "alt+p"},
        // Panel focus cycling among VISIBLE plugin panels: the focused one
        // alone receives key events (keybinding_dispatch.cpp). Not bare
        // ctrl-letters: global actions dispatch BEFORE the prompt editor, and
        // ctrl+e (the old prev default) is the editor's end-of-line -- it
        // was dead in the prompt. ctrl+alt+letter pairs with the other
        // ctrl+alt chords below and shadows no editing key.
        {"panel.focus.next", "ctrl+alt+n"},
        {"panel.focus.prev", "ctrl+alt+p"},
        {"tab.close", "ctrl+w"},
        {"chat.load_older", "pageup"},
        // Not ctrl+up/ctrl+down: macOS binds those systemwide by default
        // (Mission Control / Application Windows), intercepted before the
        // terminal ever sees them. ctrl+alt+k/j pair with chat.row_up/
        // chat.row_down's alt+k/alt+j (vim up/down) the same way
        // ctrl+alt+c/t/o below already pair with alt+c/t/o -- a coarser
        // variant of an existing alt+letter action.
        {"chat.scroll_up", "ctrl+alt+k"},
        {"chat.scroll_down", "ctrl+alt+j"},
        // Not "end": HandleGlobalKey (keybinding_dispatch.cpp) runs BEFORE the
        // prompt editor ever sees an event (app.cpp wraps session_view, which
        // wraps the editor, in the CatchEvent that dispatches these), so
        // claiming plain End here made it unreachable for the editor's own
        // cursor-to-end-of-buffer use -- Home worked (nothing claims it),
        // End silently didn't. ctrl+end/alt+end have no single-candidate
        // encoding in key_event_parser.cpp's ParseKeySpec (only unmodified
        // named keys resolve there), so binding either would parse to nothing
        // and warn at startup rather than actually fixing this. PageDown
        // pairs naturally with chat.load_older's PageUp instead, and is
        // already a parseable, unclaimed named key.
        {"chat.scroll_bottom", "pagedown"},
        // Not shift+up/down: the key parser's arrow-key branch has no
        // distinct encoding for shift+arrow (it degrades to plain arrow,
        // ParseKeySpec's own "ctrl+/alt+/plain only" comment) -- alt+j/k
        // (vim-style) are actually distinguishable events instead.
        {"chat.row_up", "alt+k"},
        {"chat.row_down", "alt+j"},
        {"chat.toggle_collapse", "alt+c"},
        // Not alt+shift+c: a modified LETTER only consults `shift` in the
        // plain case (ParseKeySpec's single-letter branch) -- combined with
        // alt it silently drops shift and collides with plain alt+c.
        // ctrl+alt+c is an actually distinct encoding (CtrlAltLetter).
        {"chat.toggle_collapse_all", "ctrl+alt+c"},
        // The per-kind fold toggles: ctrl+alt+<letter>, the encoding family
        // toggle_collapse_all already uses -- CtrlAltLetter is a single
        // distinct event, and plain alt+t/alt+o stay free for tab-bar /
        // filter actions. These flip the kind's STARTUP fold default
        // (ui.json fold_thinking/fold_tools) and persist it back through
        // SaveFoldDefaults, so the choice survives a restart.
        {"chat.toggle_thinking_fold", "ctrl+alt+t"},
        {"chat.toggle_tool_fold", "ctrl+alt+o"},
        {"chat.toggle_timestamps", "alt+t"},
        {"turn.cancel", "escape"},
        {"app.quit", "ctrl+q"},
    };
    return kDefaults;
}

// Keys handled directly by keybinding_dispatch.cpp's HandleGlobalKey (e.g.
// the hardcoded Event::CtrlC busy-cancel/idle-quit branch), never routed
// through ResolvedKeymap::Matches -- so a user/default binding to one of
// these is accepted with no parse error and simply never fires. Checked in
// ApplyDefaults() the same way a normal collision is, but never inserted
// into bindings_ since these aren't real, rebindable actions.
const std::unordered_map<std::string, std::string>& ReservedKeys() {
    static const std::unordered_map<std::string, std::string> kReserved{
        {"app cancel/quit (Event::CtrlC, keybinding_dispatch.cpp)", "ctrl+c"},
    };
    return kReserved;
}

}  // namespace

Keybindings Keybindings::Parse(std::string_view json) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("keybindings.json is not valid JSON: ") + e.what());
    }
    if (!doc.is_object()) {
        throw std::runtime_error("keybindings.json must be a JSON object of action -> key");
    }

    Keybindings out;
    std::map<std::string, std::string> key_to_action;   // duplicate-key detection
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!it.value().is_string()) {
            throw std::runtime_error("keybindings.json: value for '" + it.key() +
                                     "' must be a string");
        }
        const std::string key = it.value().get<std::string>();
        // Two actions on one key are silently ordered by source iteration today
        // -- warn, so a rebinding that shadows another is visible at startup.
        // The comparison runs on NormalizeKeySpec's COLLISION IDENTITY, not the
        // raw string: "ctrl+j" and "enter" (or "ctrl+i" and "tab") are the same
        // physical key to FTXUI even though their specs differ, and a user who
        // binds ctrl+j while another action owns Enter otherwise gets a binding
        // that silently never fires.
        const std::string identity = NormalizeKeySpec(key);
        const auto collision = key_to_action.find(identity);
        if (collision != key_to_action.end()) {
            // NOT "the first wins": bindings_ keeps every action's own key
            // independently (below), so both actions stay configured to this
            // key. Which one actually fires is decided by
            // keybinding_dispatch.cpp's HandleGlobalKey -- a fixed if-chain
            // checked in source order, unrelated to JSON iteration order.
            common::DefaultLogger().warn(
                "keybindings.json: '{}' ({}) is bound to both '{}' and '{}' -- only one can "
                "fire at runtime, decided by keybinding_dispatch.cpp's dispatch order, not by "
                "this file",
                key, identity, collision->second, it.key());
        } else {
            key_to_action.emplace(identity, it.key());
        }
        // Unknown action names are KEPT: a client plugin (E5) registers views
        // addressable from here by runtime-chosen ids, and this file may predate
        // or postdate any given build. See the header comment.
        out.bindings_[it.key()] = key;
    }
    return out;
}

Keybindings Keybindings::Load(const std::filesystem::path& config_dir) {
    const auto path = config_dir / "keybindings.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            // NOT the same as "no file": a stat error (permission denied on
            // the config dir, a dangling symlink loop) silently falling into
            // the fresh-dir path hid the real problem behind a working-
            // looking default setup.
            common::DefaultLogger().warn(
                "could not stat keybindings.json ({}); using default keybindings",
                ec.message());
        }
        // A fresh config dir is the normal first-run state: the defaults stand.
        Keybindings out;
        out.bindings_ = DefaultBindings();
        return out;
    }
    std::ifstream in(path);
    if (!in) throw std::runtime_error("keybindings.json exists but cannot be read: " + path.string());
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    Keybindings parsed = Parse(contents);
    // Defaults fill actions the user's file does not mention, so an upgrade that
    // adds a core action still works without rewriting anyone's file. The user's
    // own binding always wins over the default for the same action.
    parsed.ApplyDefaults();
    return parsed;
}

void Keybindings::ApplyDefaults() {
    // emplace, not assignment: the user's own binding always wins over the
    // default for the same action.
    for (const auto& [action, key] : DefaultBindings()) {
        // CROSS-SOURCE collision warning: Parse only sees the user's file, so
        // a user key that fights a DEFAULT over the same physical key was
        // invisible -- the dispatch order (user table built first, defaults
        // emplaced into it) decides silently, exactly like the in-file case
        // above. Warning-only: the winning binding is unchanged.
        const std::string identity = NormalizeKeySpec(key);
        for (const auto& [existing_action, existing_key] : bindings_) {
            if (existing_action != action && NormalizeKeySpec(existing_key) == identity) {
                // NOT "the user binding wins": both actions stay independently
                // bound to this key in bindings_ (emplace below never
                // overwrites `existing_action`'s entry). Which one actually
                // fires is decided by keybinding_dispatch.cpp's fixed dispatch
                // order, not by which table this loop happened to insert from.
                common::DefaultLogger().warn(
                    "keybindings.json: default '{}' for '{}' collides with user binding for "
                    "'{}' on the same key -- only one can fire at runtime, decided by "
                    "keybinding_dispatch.cpp's dispatch order, not by this file",
                    key, action, existing_action);
            }
        }
        bindings_.emplace(action, key);
    }
    // Keys HandleGlobalKey consumes directly (keybinding_dispatch.cpp),
    // bypassing the ResolvedKeymap dispatch entirely -- not real actions, so
    // they're never inserted into bindings_, but a user/default binding on
    // one of these keys is silently unreachable. Warn the same way an
    // ordinary collision does.
    for (const auto& [reserved_name, reserved_key] : ReservedKeys()) {
        const std::string identity = NormalizeKeySpec(reserved_key);
        for (const auto& [existing_action, existing_key] : bindings_) {
            if (NormalizeKeySpec(existing_key) == identity) {
                common::DefaultLogger().warn(
                    "keybindings.json: '{}' bound to '{}' can never fire -- {} is reserved for "
                    "{}, consumed before any action dispatch",
                    existing_key, existing_action, reserved_key, reserved_name);
            }
        }
    }
}

std::string Keybindings::KeyFor(std::string_view action, std::string_view fallback) const {
    if (const auto it = bindings_.find(std::string(action)); it != bindings_.end()) {
        return it->second;
    }
    return std::string(fallback);
}

bool Keybindings::EnsureDefault(std::string_view action, std::string_view key) {
    if (key.empty()) return false;
    const std::string name(action);
    if (bindings_.find(name) != bindings_.end()) return false;
    bindings_.emplace(name, std::string(key));
    return true;
}

}  // namespace minimal::client
