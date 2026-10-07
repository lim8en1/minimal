#pragma once

// `minimal status-bar [--width N]...`: the status bar as this TUI would
// compose it, printed to a shell -- for trying placements without starting
// a session, and as the start of a hand-edited ui.json.
//
// Loads the TUI plugins as the TUI does (LoadClientPluginEnvironment, no
// server: every ClientApi call answers nothing), composes the bar, and
// prints
//   - the layout: the shown segment ids in order, split marked;
//   - per width, the bar rendered from a canned tab -- a segment with
//     nothing to show without a server (cost-view's) stands in as {id} --
//     with the same priorities, caps and drops the TUI applies, and what
//     dropped;
//   - a complete "status_segments" block, every registered segment's
//     effective placement (false: shown nowhere), to paste into ui.json.

#include <string>
#include <vector>

namespace minimal::client {

// `args`: everything after "status-bar". Returns the process exit code.
int RunStatusBarCommand(const std::vector<std::string>& args);

}  // namespace minimal::client
