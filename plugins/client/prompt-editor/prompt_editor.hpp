#pragma once

// The prompt editor: a multiline input plus the two live-suggestion surfaces.
//
// NEWLINE: plain Enter SUBMITS; Alt+Enter inserts a newline. Alt+Enter (not
// Shift+Enter) is the default because terminals disagree on what they can
// encode -- Shift+Enter is indistinguishable from Enter without the kitty
// keyboard protocol / CSI-u (ARCHITECTURE.md "Customization surface"). The
// alt sequence is matched in every encoding a terminal actually emits: ESC CR
// / ESC LF, the kitty protocol's CSI 13;{mod}u, and xterm modifyOtherKeys'
// CSI 27;{mod};13~ (see key_event_parser.cpp's enter/return branch). A
// terminal that reports Option+Enter as a PLAIN \r -- the macOS default in
// Terminal.app, iTerm2, Ghostty, Alacritty and WezTerm until option-as-alt /
// the kitty protocol is enabled -- cannot be distinguished from submit by
// any in-app match; enable the terminal setting, or remap the
// "prompt.newline" action in keybindings.json (e.g. to "shift+enter", or to
// any key unrelated to Enter) -- the editor asks PromptEditorHost::Matches,
// so the rebind takes effect.
//
// UNDO: alt+z undoes the last edit, ctrl+alt+z redoes it (PromptUndo below).
// Not ctrl+z: that is the terminal's suspend key.
//
// SOFT WRAP: the prompt body is rendered from WrapPromptContent's visual
// lines (word-aware, to the terminal width, at most half the screen tall,
// scrolled to keep the caret visible) instead of FTXUI Input's one-row-per-
// logical-line render, so a long prompt can be reviewed before sending. Only
// the rendering moved: the Input component still owns every key event, and
// click-to-position is re-implemented from the render geometry.
//
// @-MENTIONS: typing '@' arms the inline suggestion list (a bare '@' is a
// live query too, but its empty prefix always resolves to an empty answer,
// so the list opens once a character follows the '@'), queried through the
// SuggestPaths RPC debounced ~100-150 ms (server-resolved, so every frontend
// inherits the strategy). Hand-rolled inline list, NOT ftxui::Dropdown -- that
// is a click-to-open selector; type-ahead reads better inline. Tab completes
// the highlighted suggestion.
//
// /-COMMANDS: a prompt starting with '/' goes out as a CommandInvocation.
// /-AUTOCOMPLETE (E5): while the first token still reads like a command name
// ('/' plus non-space), the local ListCommands catalog filters inline -- no
// debounce, the catalog is an in-memory snapshot (CommandCatalogCache
// refreshes it in the background, per tab scope). Prompt
// shorthands are annotated as the one kind that will submit a turn
// (docs/EXTENSIBILITY.md 4.4), so a macro never surprises anyone.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::client::prompt_editor {

// The prompt's undo/redo history ("prompt.undo" / "prompt.redo"): bounded
// stacks of {content, caret} snapshots. MakePromptEditor records the text as
// it was BEFORE each edit, and coalesces runs so one undo reverts a word, not
// a keystroke: consecutive inserts (or consecutive erases) at a contiguous
// caret merge into one step until whitespace is typed, the caret jumps, or
// more than kUndoCoalesceGap passes. A whole bracketed paste is one step; every
// other edit (kill line, completion accept, history recall, newline) is a
// step of its own. Submit clears both stacks. Loop-thread only.
class PromptUndo {
public:
    struct Snapshot {
        std::string content;
        int cursor = 0;
    };
    enum class Kind { Insert, Erase, Paste, Other };


    // An edit of `kind` turned `before` into text whose caret is now at
    // `cursor_after`. `breaks_run` forces a new step (e.g. a typed space).
    // Any new edit drops the redo stack.
    void Record(Snapshot before, Kind kind, int cursor_after, bool breaks_run,
                std::chrono::steady_clock::time_point now);
    // The caret moved without an edit: the next edit starts a new step.
    void BreakRun() { run_open_ = false; }

    // Swap `current` for the previous (next) snapshot; nullopt when there is
    // none. `current` goes onto the opposite stack.
    std::optional<Snapshot> Undo(Snapshot current);
    std::optional<Snapshot> Redo(Snapshot current);

    // Drops both stacks (submit). Bumps generation(), which is how the editor
    // tells "this event submitted" from "this event edited".
    void Clear();
    std::uint64_t generation() const { return generation_; }

    std::size_t undo_depth() const { return undo_.size(); }
    std::size_t redo_depth() const { return redo_.size(); }

private:
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool run_open_ = false;
    Kind run_kind_ = Kind::Other;
    int run_cursor_ = 0;
    std::chrono::steady_clock::time_point run_at_{};
    std::uint64_t generation_ = 0;
};

// One per tab's editor (the plugin's PromptEditor instance owns it).
struct PromptEditorState {
    std::string content;                    // the input's text (InputOption::content)
    // Bound to the Input's InputOption::cursor_position. Only written
    // explicitly by accept_suggestion (prompt_editor.cpp) today -- FTXUI
    // itself only CLAMPS this into content's bounds after an external splice,
    // it never moves it, so a completion that changes content's length
    // without touching this field leaves the cursor mid-word.
    int cursor_position = 0;
    std::vector<std::string> suggestions;   // live suggestion list (display text)
    std::vector<std::string> completions;   // what Tab inserts (differs from display for commands)
    int suggestion_selected = 0;
    bool list_open = false;
    // Debounce bookkeeping for the SuggestPaths query.
    std::string pending_prefix;
    std::chrono::steady_clock::time_point query_at{};
    bool query_pending = false;
    // A CROSS-THREAD-SAFE mirror of query_pending, kept in sync at every site
    // that writes query_pending -- NOT itself authoritative (query_pending
    // is, and is read/written only on the FTXUI loop thread like every other
    // field here). RunTui's redraw pump runs on its own thread and needs to
    // know "is a debounced query armed" (PromptEditor::WantsFrames) to
    // keep frames coming (the debounce fires from a render pass -- see
    // prompt_editor.cpp's render-time gate -- so without frames the timer
    // never elapses). A plain bool read cross-thread would be a data race;
    // this field exists solely to make that read well-defined.
    std::atomic<bool> query_pending_hint{false};
    // The browse POSITION into the app-scoped timeline (PromptTimeline: a
    // sequence number, so another tab's Record trimming the front cannot
    // shift it), and the draft it interrupted -- per tab, since they
    // read/write this state's own `content`.
    std::optional<std::uint64_t> history_cursor;
    std::string history_draft;
    // Per-tab like the draft it reverts.
    PromptUndo undo;
};

// The editor over `state`, talking to core through `host` (submit paths,
// command catalog, @-mention answers, the shared history, the keymap).
// `pasting` is the bracketed-paste flag (null: a private one); the instance
// reads it for Claims. `state`, `host` and `theme` outlive the component.
ftxui::Component MakePromptEditor(const Theme& theme, PromptEditorState& state,
                                  plugins::PromptEditorHost& host,
                                  std::shared_ptr<bool> pasting = nullptr);

}  // namespace minimal::client::prompt_editor
