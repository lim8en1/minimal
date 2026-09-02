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
#include <vector>

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

}  // namespace minimal::client
