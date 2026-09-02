#pragma once

// Pure tab-set computations backing the TUI's global keybindings
// (app.cpp "global keybindings"). Extracted specifically because this is
// where TWO real bugs lived, both in exactly this kind of index/scan math:
// the attention-jump scan starting from a fixed index instead of the focused
// tab (so it could jump backwards, or land back on the very tab that was
// Waiting), and the group-navigation cycle needing the same "find my own
// index among a filtered subset" shape. `RunTui` is a single ~1100-line
// function with no test surface reachable through it (three of the TUI's
// worst bugs shipped and went unnoticed for a full milestone specifically
// because nothing there is unit-testable); pulling the PURE parts out into
// functions over `AppState::TabSnapshot` -- a value type, no FTXUI, no
// AppState instance required -- means this class of bug is now a directly
// testable one instead of something only a person at a keyboard finds.
//
// None of this touches AppState or FTXUI: every function here is
// (tabs, ...) -> result, driven entirely by the value snapshot app.cpp
// already takes every frame.

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>

#include "minimal/client/keybindings.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// group.next / group.previous: the next (or previous) ROOT tab to focus,
// cycling among root tabs only (subagent tabs are not group boundaries).
// "" if there are no root tabs. `focused` not matching any root tab
// (including "" or a subagent id) starts the cycle at the first root.
std::string NextRootGroup(const std::vector<AppState::TabSnapshot>& tabs,
                          const std::string& focused, bool forward);

// jump.attention: the next tab carrying the attention glyph ('!'), scanning
// forward from JUST AFTER the focused tab and wrapping across every tab
// (roots and subagents alike -- attention crosses groups). "" if none
// qualify, including when the only qualifying tab is the focused one itself.
std::string NextAttentionTab(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused);

// True if any of `ids` is Thinking, Working, or Waiting -- the busy states
// that make closing/quitting destructive enough to confirm.
bool AnyWorkInFlight(const std::vector<AppState::TabSnapshot>& tabs,
                    const std::vector<std::string>& ids);

// tab.close: the session ids to close for `sid`, in CLOSE order (children
// first, root last -- so a confirmation or a close loop can act on the list
// directly without re-deriving order). Closing a root tab closes its whole
// group; closing a subagent tab closes only itself. Empty if `sid` names no
// known tab.
std::vector<std::string> TabsToClose(const std::vector<AppState::TabSnapshot>& tabs,
                                     const std::string& sid);

// The parsed form of a Keybindings document, built ONCE after keybindings.json
// (plus every plugin view's EnsureDefault) has settled, rather than re-parsed
// per event. `with_keys` (app.cpp's global-keybinding CatchEvent) used to do
// `MatchesAny(event, KeyToEvents(keys.KeyFor(action)))` per action per EVENT
// -- ~13 core actions plus one per plugin view, evaluated on every keystroke
// AND every 20 Hz pump tick -- and each KeyFor allocates a std::string,
// each ParseKeySpec lowercases into a new string, splits it into a
// vector<string>, and builds Event objects with their own string payloads.
// A table never changes after startup; re-deriving it per event was pure
// waste for a lookup that reduces to a hash probe plus a linear compare
// against (almost always) one or two candidate Events.
class ResolvedKeymap {
public:
    // Parses every action `keys` binds (core defaults, the user's file, and
    // any plugin-view EnsureDefault entries already folded in by the time
    // this runs -- see app.cpp's load ordering comment). An action whose key
    // spec parses to no known Event is recorded in Unmapped() rather than
    // silently matching nothing forever with no diagnostic.
    static ResolvedKeymap Build(const Keybindings& keys);

    // Whether `event` matches ANY of `action`'s parsed candidate Events. An
    // action Build() never saw (never bound, or bound to an unparseable key)
    // matches nothing -- the same "no mapping" outcome KeyFor's empty
    // fallback produced before.
    bool Matches(std::string_view action, const ftxui::Event& event) const;

    // (action, key) pairs whose key spec parsed to zero candidate Events --
    // the startup-warning condition ParseKeySpec's own header documents
    // ("a name the terminal cannot encode matches nothing"). Named PAIRS,
    // not just actions, so a caller's warning can name the offending key
    // spec too, same as the loop this replaces.
    const std::vector<std::pair<std::string, std::string>>& Unmapped() const {
        return unmapped_;
    }

private:
    std::unordered_map<std::string, std::vector<ftxui::Event>> table_;
    std::vector<std::pair<std::string, std::string>> unmapped_;
};

}  // namespace minimal::client
