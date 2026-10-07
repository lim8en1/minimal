#pragma once

// ui.json: the client's UI config, seeded alongside keybindings.json and
// governed by the same rules -- degrade-don't-throw (a broken file is warned
// about and defaults applied, never fatal: it can only affect decoration),
// unknown keys kept by the round-trip... there is no rewrite here (the file is
// user-owned, seeded once), so "kept" means simply ignored.
//
// Keys:
//   status_format -- the status-bar format string ("{version} | {tokens}").
//     Load-bearing plugin infrastructure (ARCHITECTURE.md "Client plugins"):
//     it is how plugin-registered segments are addressed, so ids that exist
//     only at plugin load time are legal here. Unknown ids render nothing with
//     a startup warning.
//   scrollback_window_max_messages -- the per-tab scrollback window's message
//     cap (track R2: the window is a bounded sliding window, not an
//     unbounded append; loading older pages beyond the cap evicts the
//     newest-loaded pages, flagged in the view).
//   tool_output_buffer_bytes -- the per-tool live-stdout ring buffer's byte
//     cap, applied at append time (head + marker + tail), mirroring the
//     server's persisted-result cap.
//   theme -- the name of a themes/<name>.json file (without extension), e.g.
//     "dark" or "light". A name Theme::Load cannot find falls back to the
//     terminal-default theme with a startup warning, the same degrade rule
//     as everything else here.
//   fold_thinking / fold_tools -- the STARTUP auto-fold default for thinking
//     and tool-activity rows: "auto" (the built-in kind default),
//     "expanded" or "collapsed". Anything else is warned about and ignored,
//     the same degrade rule as everything else here. Runtime toggles
//     (chat.toggle_thinking_fold / chat.toggle_tool_fold) persist back
//     through SaveFoldDefaults -- part of the ONE managed write below.
//
// THE ONE MANAGED WRITE. ui.json is otherwise user-owned and never rewritten
// by the client -- the exceptions are SaveThemePreference (which /theme uses
// to persist a runtime switch) and SaveFoldDefaults (which the fold toggles
// use to persist theirs). Both round-trip the whole document (every other
// key survives verbatim) and refuse to touch a file they cannot parse, the
// same rule client-plugins.json's writer follows.

#include <filesystem>
#include <optional>
#include <string>

namespace minimal::client {

// The fold-default vocabulary: 0 = "auto" (the built-in kind default,
// CollapsedByDefault), 1 = "expanded", 2 = "collapsed". Strings in ui.json,
// ints in memory.
enum : int { kFoldDefaultAuto, kFoldExpanded, kFoldCollapsed };

struct UiConfig {
    // Empty = use the built-in default. A non-string or missing key means the
    // same thing; a malformed FILE (not valid JSON) is warned and defaulted.
    std::string status_format;

    // 0/missing = built-in default. Non-positive-int values are warned and
    // defaulted.
    int scrollback_window_max_messages = 0;
    int tool_output_buffer_bytes = 0;
    // The tools panel's (and every plugin panel's) height in rows; 0 (unset)
    // = the built-in 12. ui.json values below 3 are ignored.
    int panel_height = 0;

    // The startup auto-fold default for Thinking / ToolActivity rows:
    // kFoldDefaultAuto = built-in kind default (CollapsedByDefault),
    // kFoldExpanded = start expanded, kFoldCollapsed = start collapsed.
    // Anything unparsable in the file is warned and left at auto.
    int fold_thinking = kFoldDefaultAuto;
    int fold_tools = kFoldDefaultAuto;

    // Empty = "dark" (the built-in default).
    std::string theme;
};

UiConfig LoadUiConfig(const std::filesystem::path& config_dir);

// Sets the "theme" key in <config_dir>/ui.json to `name`, preserving every
// other key. Best-effort: a malformed ui.json, an unwritable config dir or a
// failed rename is warned about through the logger and otherwise ignored --
// the in-session theme switch has already applied, this only records it for
// the next launch. Creates the config dir when missing.
void SaveThemePreference(const std::filesystem::path& config_dir, const std::string& name);

// The fold-toggle counterpart: sets "fold_thinking"/"fold_tools" to
// "expanded"/"collapsed" (kFoldDefaultAuto round-trips as "auto"), preserving
// every other key. A nullopt argument leaves that key untouched -- the OTHER
// fold toggle did not run this session. Same best-effort discipline as
// SaveThemePreference: the in-session toggles have already applied, this only
// records them for the next launch. Creates the config dir when missing.
// Returns whether the file was written -- the toggle's feedback notice says
// "(saved)" only when this landed (a malformed/unreadable ui.json is warned
// about and left untouched).
bool SaveFoldDefaults(const std::filesystem::path& config_dir,
                      std::optional<int> thinking, std::optional<int> tools);

}  // namespace minimal::client
