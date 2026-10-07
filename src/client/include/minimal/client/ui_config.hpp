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
//
// THE ONE MANAGED WRITE. ui.json is otherwise user-owned and never rewritten
// by the client -- the exception is SaveThemePreference (which /theme uses to
// persist a runtime switch). It round-trips the whole document (every other
// key survives verbatim) and refuses to touch a file it cannot parse, the
// same rule client-plugins.json's writer follows.

#include <filesystem>
#include <string>

namespace minimal::client {

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

}  // namespace minimal::client
