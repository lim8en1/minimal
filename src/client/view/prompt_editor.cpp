#include "minimal/client/view/prompt_editor.hpp"

#include <algorithm>
#include <memory>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/config_file_io.hpp"
#include "minimal/client/key_event_parser.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/view/prompt_wrap.hpp"
#include "minimal/common/logger.hpp"

#include <string>
#include <vector>

namespace minimal::client {

namespace {

using ftxui::Element;
using ftxui::Event;

constexpr auto kSuggestDebounce = std::chrono::milliseconds(120);

// The prompt's own gutter: "❯ " on the first line, two spaces under it. The
// wrap width and the click-to-position mapping both account for it.
constexpr int kPromptGutter = 2;

constexpr const char* kPromptPlaceholder =
    "type here; @ for files, / for commands, alt+enter for newline";

// The events that INSERT a newline rather than submit. Alt+Enter is the
// portable multi-line key, encoded ESC CR (most terminals) or ESC LF; the
// CSI-u rows cover terminals that report Enter with a modifier attached --
// the kitty keyboard protocol's \x1b[13;{mod}u and xterm modifyOtherKeys'
// \x1b[27;{mod};13~ (FTXUI 7 delivers unknown CSI as Event::Special with the
// raw bytes preserved -- the same mechanism the bracketed-paste markers use,
// and the one ctrl+tab's \x1b[27;5;9~ already arrives through). On macOS
// several terminals send Option+Enter as a PLAIN \r unless their
// option-as-alt / kitty-protocol setting is on -- no byte distinguishes it
// from submit then, and no in-app match can fix that (see prompt_editor.hpp).
bool IsNewlineEvent(const Event& event) {
    static const Event kAltEnterCr = Event::Character("\x1b\r");
    static const Event kAltEnterLf = Event::Character("\x1b\n");
    static const Event kKittyAltEnter = Event::Special("\x1b[13;3u");
    static const Event kKittyShiftEnter = Event::Special("\x1b[13;2u");
    static const Event kModifyOtherKeysAltEnter = Event::Special("\x1b[27;3;13~");
    static const Event kModifyOtherKeysShiftEnter = Event::Special("\x1b[27;2;13~");
    return event == kAltEnterCr || event == kAltEnterLf || event == kKittyAltEnter ||
           event == kKittyShiftEnter || event == kModifyOtherKeysAltEnter ||
           event == kModifyOtherKeysShiftEnter;
}

// Newline AT THE CURSOR, not appended to the end: appending always broke a
// newline inserted while editing mid-line (the common case -- Alt+Enter is
// how a multi-line prompt gets built up one line at a time) into a line
// dropped at the tail instead. cursor_position is clamped defensively --
// FTXUI only clamps it INTO content's bounds after an external splice, it
// never derives it, so a stale value from before some other edit must not be
// trusted past content's own length.
void InsertNewlineAtCursor(PromptEditorState& state) {
    const auto pos = std::clamp<std::size_t>(
        static_cast<std::size_t>(state.cursor_position), 0, state.content.size());
    state.content.insert(pos, "\n");
    state.cursor_position = static_cast<int>(pos) + 1;
}

// Render-pass geometry shared with the mouse handler below: where the wrapped
// rows landed on screen and which content slice they show. Written by the
// Renderer every frame (its vbox is reflected into `box`), read on mouse
// events -- the wrapped Input's own click-to-position is out of the loop now
// that its Render() output is no longer what the frame draws.
struct PromptMouseMap {
    ftxui::Box box;
    std::size_t first_row = 0;
    PromptWrapResult wrap;
};

// The start index of the '@' token the CURSOR currently sits inside (or at
// the end of), or nullopt if none. Scans backward from `cursor`: an '@' hit
// before any whitespace means the cursor is inside a live mention token;
// whitespace hit first (or the buffer start reached with neither) means it
// isn't. Anchored at the cursor, not `text.rfind('@')` over the WHOLE
// buffer -- editing an EARLIER '@' with the cursor placed back inside it
// used to resolve to whichever '@' was LAST in the buffer, regardless of
// where the cursor actually was.
std::optional<std::size_t> MentionTokenStart(const std::string& text, int cursor) {
    const std::size_t pos = std::clamp<std::size_t>(
        cursor < 0 ? std::size_t{0} : static_cast<std::size_t>(cursor), std::size_t{0},
        text.size());
    std::size_t i = pos;
    while (i > 0) {
        const char c = text[i - 1];
        if (c == '@') return i - 1;
        if (std::isspace(static_cast<unsigned char>(c))) return std::nullopt;
        --i;
    }
    return std::nullopt;
}

// Where the token starting at `at` (an '@') ends: the next whitespace, or
// the end of the buffer. Only accept_suggestion needs this -- MentionPrefixAt
// (below) only ever needs up TO the cursor, not the whole token.
std::size_t MentionTokenEnd(const std::string& text, std::size_t at) {
    std::size_t i = at + 1;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    return i;
}

// The token being completed: the text between the '@' MentionTokenStart
// found and the cursor -- what the user has typed SO FAR, which is what
// narrows the suggestion list. Bash-completion shape: a mention is '@' +
// non-space characters.
std::string MentionPrefixAt(const std::string& text, int cursor) {
    const auto at = MentionTokenStart(text, cursor);
    if (!at.has_value()) return "";
    const std::size_t pos = std::clamp<std::size_t>(
        cursor < 0 ? std::size_t{0} : static_cast<std::size_t>(cursor), std::size_t{0},
        text.size());
    return text.substr(*at + 1, pos - *at - 1);
}

// True while `text` is STILL a command name being typed: '/' plus zero or
// more non-space characters, and nothing else. Once whitespace lands the
// command is chosen and editing its arguments is nobody's autocomplete
// business. "Zero" is deliberate -- a bare "/" is the START of typing a
// command name, not a finished non-command line -- see CommandPrefixOf below,
// which used to conflate that with "not a command line at all" (both
// returned "").
bool IsCommandNameInProgress(const std::string& text) {
    if (text.empty() || text.front() != '/') return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

// The command name typed so far, once IsCommandNameInProgress is true (an
// empty result then means "match every command", not "not a command line").
std::string CommandPrefixOf(const std::string& text) {
    if (text.empty() || text.front() != '/') return "";
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) return "";
    }
    return text.substr(1);
}

// The editor's keymap actions at their default keys, for an editor built
// without a keymap (see the header's `key_matches` comment).
bool DefaultPromptKeyMatches(std::string_view action, const Event& event) {
    static const std::vector<Event> kUndo = ParseKeySpec("alt+z");
    static const std::vector<Event> kRedo = ParseKeySpec("ctrl+alt+z");
    const auto any_of = [&event](const std::vector<Event>& keys) {
        return std::find(keys.begin(), keys.end(), event) != keys.end();
    };
    if (action == "prompt.newline") return IsNewlineEvent(event);
    if (action == "prompt.undo") return any_of(kUndo);
    if (action == "prompt.redo") return any_of(kRedo);
    return false;
}

// Undo/redo around the whole editor (PromptUndo). A ComponentBase subclass,
// not a CatchEvent: recording needs the text AFTER the editor's handlers and
// the Input have applied the event, and a CatchEvent runs before its child.
// Its one child stays registered, so focus resolves through it unchanged.
class UndoableEditor : public ftxui::ComponentBase {
public:
    UndoableEditor(ftxui::Component inner, PromptEditorState& state,
                   std::shared_ptr<bool> pasting,
                   std::function<bool(std::string_view, const Event&)> key_matches)
        : state_(state), pasting_(std::move(pasting)), key_matches_(std::move(key_matches)) {
        Add(std::move(inner));
    }

    bool OnEvent(Event event) override {
        // Not mid-paste: a pasted byte that happens to equal the undo key
        // (a macOS Option glyph) is content.
        if (!*pasting_ && !event.is_mouse()) {
            const bool undo = key_matches_("prompt.undo", event);
            if (undo || key_matches_("prompt.redo", event)) {
                PromptUndo::Snapshot current{state_.content, state_.cursor_position};
                auto target = undo ? state_.undo.Undo(std::move(current))
                                   : state_.undo.Redo(std::move(current));
                if (target) Restore(std::move(*target));
                return true;   // consumed even with nothing to undo: never typed as text
            }
        }

        PromptUndo::Snapshot before{state_.content, state_.cursor_position};
        const std::uint64_t generation = state_.undo.generation();
        const bool was_pasting = *pasting_;
        const bool handled = ComponentBase::OnEvent(event);
        // Submitted: the stacks were cleared, and the line is gone for good.
        if (state_.undo.generation() != generation) return handled;
        // A new paste never merges into the previous one.
        if (!was_pasting && *pasting_) state_.undo.BreakRun();
        if (state_.content == before.content) {
            if (state_.cursor_position != before.cursor) state_.undo.BreakRun();
            return handled;
        }
        const PromptUndo::Kind kind = KindOf(event, before.content.size());
        const bool breaks_run =
            kind == PromptUndo::Kind::Insert &&
            std::isspace(static_cast<unsigned char>(event.character().front()));
        state_.undo.Record(std::move(before), kind, state_.cursor_position, breaks_run,
                           std::chrono::steady_clock::now());
        return handled;
    }

private:
    PromptUndo::Kind KindOf(const Event& event, std::size_t size_before) const {
        if (*pasting_) return PromptUndo::Kind::Paste;
        if (event == Event::Backspace || event == Event::Delete) return PromptUndo::Kind::Erase;
        // A typed printable character (the ctrl+letter bytes are "characters"
        // to FTXUI too, and kill-line is its own step).
        if (event.is_character() && !event.character().empty() &&
            state_.content.size() > size_before) {
            const auto first = static_cast<unsigned char>(event.character().front());
            if (first >= 0x20 && first != 0x7f) return PromptUndo::Kind::Insert;
        }
        return PromptUndo::Kind::Other;
    }

    void Restore(PromptUndo::Snapshot snapshot) {
        state_.content = std::move(snapshot.content);
        state_.cursor_position =
            std::clamp(snapshot.cursor, 0, static_cast<int>(state_.content.size()));
        // The restored text is no longer the recalled history line.
        state_.history_cursor.reset();
    }

    PromptEditorState& state_;
    std::shared_ptr<bool> pasting_;
    std::function<bool(std::string_view, const Event&)> key_matches_;
};

}  // namespace

void PromptUndo::Record(Snapshot before, Kind kind, int cursor_after, bool breaks_run,
                        std::chrono::steady_clock::time_point now) {
    redo_.clear();
    const bool coalesce =
        run_open_ && !breaks_run && kind != Kind::Other && kind == run_kind_ &&
        (kind == Kind::Paste || (before.cursor == run_cursor_ && now - run_at_ <= kCoalesceGap));
    if (!coalesce) {
        undo_.push_back(std::move(before));
        if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    }
    run_open_ = kind != Kind::Other;
    run_kind_ = kind;
    run_cursor_ = cursor_after;
    run_at_ = now;
}

std::optional<PromptUndo::Snapshot> PromptUndo::Undo(Snapshot current) {
    if (undo_.empty()) return std::nullopt;
    redo_.push_back(std::move(current));
    Snapshot target = std::move(undo_.back());
    undo_.pop_back();
    run_open_ = false;
    return target;
}

std::optional<PromptUndo::Snapshot> PromptUndo::Redo(Snapshot current) {
    if (redo_.empty()) return std::nullopt;
    undo_.push_back(std::move(current));
    Snapshot target = std::move(redo_.back());
    redo_.pop_back();
    run_open_ = false;
    return target;
}

void PromptUndo::Clear() {
    undo_.clear();
    redo_.clear();
    run_open_ = false;
    ++generation_;
}

namespace {
// 500 recent entries is far past what arrow-up recall is used for.
constexpr std::size_t kMaxHistoryEntries = 500;
}  // namespace

void PromptHistory::Record(const std::string& line) {
    // Skip a consecutive duplicate of the last entry -- repeatedly
    // submitting the same line, or repeating what recall just brought back,
    // should not grow the list every time.
    if (!entries.empty() && entries.back() == line) {
        return;
    }
    entries.push_back(line);
    // Bounded: a long-lived TUI session otherwise kept every submitted line
    // for the process's whole life. 500 recent entries is far past what
    // arrow-up recall is used for.
    if (entries.size() > kMaxHistoryEntries) {
        const std::size_t trimmed = entries.size() - kMaxHistoryEntries;
        entries.erase(entries.begin(), entries.begin() + static_cast<long>(trimmed));
        // Every registered browse position is a raw index into `entries`
        // (see the ArrowUp/ArrowDown handling below); a front-trim triggered
        // by ANY tab's Record() call must shift every one of them by the
        // same amount, or a DIFFERENT tab mid-browse silently ends up
        // pointed at an unrelated (or out-of-range) line.
        for (std::optional<std::size_t>* cursor : cursors_) {
            if (!cursor->has_value()) continue;
            if (**cursor >= trimmed) {
                **cursor -= trimmed;
            } else {
                cursor->reset();
            }
        }
    }
    Save();
}

void PromptHistory::PersistTo(std::filesystem::path path) {
    persist_path_ = std::move(path);
    std::string text;
    if (!TryReadConfigFile(persist_path_, &text)) return;
    const auto doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_array()) return;
    std::vector<std::string> loaded;
    for (const auto& line : doc) {
        if (line.is_string()) loaded.push_back(line.get<std::string>());
    }
    // Loaded lines come BEFORE anything recorded this run, under the same
    // entry bound Record keeps.
    entries.insert(entries.begin(), loaded.begin(), loaded.end());
    if (entries.size() > kMaxHistoryEntries) {
        entries.erase(entries.begin(),
                      entries.begin() + static_cast<long>(entries.size() - kMaxHistoryEntries));
    }
}

void PromptHistory::Save() const {
    if (persist_path_.empty()) return;
    // The newest entries that fit the byte budget, oldest first on disk.
    std::size_t bytes = 0;
    std::size_t first = entries.size();
    while (first > 0 && bytes + entries[first - 1].size() <= kPersistMaxBytes) {
        bytes += entries[first - 1].size();
        --first;
    }
    nlohmann::json doc = nlohmann::json::array();
    for (std::size_t i = first; i < entries.size(); ++i) doc.push_back(entries[i]);
    if (!WriteConfigFileAtomic(persist_path_,
                               doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
        common::DefaultLogger().warn("prompt history: could not write {}",
                                     persist_path_.string());
    }
}

void PromptHistory::RegisterCursor(std::optional<std::size_t>* cursor) {
    cursors_.push_back(cursor);
}

void PromptHistory::UnregisterCursor(std::optional<std::size_t>* cursor) {
    cursors_.erase(std::remove(cursors_.begin(), cursors_.end(), cursor), cursors_.end());
}

ftxui::Component MakePromptEditor(
    const Theme& theme, PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command,
    const MentionSuggestor& suggest,
    CommandCatalogSource command_catalog,
    std::function<bool()> session_available,
    std::function<bool()> subagent_read_only,
    std::shared_ptr<bool> pasting, PromptHistory* history,
    std::function<bool(std::string_view action, const ftxui::Event&)> key_matches) {
    using namespace ftxui;
    // Null (every test call site): the default keys. RunTui passes the
    // keymap instead (see the header's `key_matches` comment).
    if (!key_matches) key_matches = DefaultPromptKeyMatches;
    const std::function<bool(const Event&)> newline_check =
        [key_matches](const Event& event) { return key_matches("prompt.newline", event); };
    // Null (every pre-existing call site): a private timeline, same behavior
    // as before, no cross-editor sharing. OWNED BY THE CLOSURES (a shared_ptr
    // captured below), never a factory stack local -- the component outlives
    // this call, and a reference to a dangled local is exactly the trap the
    // capture-list comment below documents for on_text/on_command.
    std::shared_ptr<PromptHistory> owned_history =
        history != nullptr ? nullptr : std::make_shared<PromptHistory>();
    PromptHistory& hist = history != nullptr ? *history : *owned_history;

    // Only a SHARED history (history != nullptr, i.e. per-tab editors over
    // one app-scoped timeline) needs this: a private/owned history has
    // exactly one editor, so nothing else can trim it out from under this
    // editor's own browse position. `state.history_cursor`'s address is
    // stable for the component's lifetime (PerTabPromptEditors::states_ is a
    // std::map, and ForgetTab drops state and editor together), so
    // registering it here and unregistering via this guard's destruction --
    // tied to the returned component's lifetime below -- brackets it exactly.
    std::shared_ptr<void> history_cursor_guard;
    if (history != nullptr) {
        history->RegisterCursor(&state.history_cursor);
        history_cursor_guard = std::shared_ptr<void>(nullptr, [history, cursor = &state.history_cursor](void*) {
            history->UnregisterCursor(cursor);
        });
    }

    // Multiline ON: Return-as-newline is the Input's default, so the submit
    // decision is ours -- CatchEvent sees every event first. cursor_position
    // is bound explicitly (not the 2-arg factory) so accept_suggestion below
    // can move the caret after splicing in a completion -- FTXUI's own
    // InputOption otherwise owns that position privately, unreachable from
    // here, and only clamps it into range rather than moving it.
    InputOption input_option;
    input_option.content = &state.content;
    input_option.placeholder = kPromptPlaceholder;
    input_option.cursor_position = &state.cursor_position;
    auto input = Input(input_option);

    // Render/mouse shared geometry (see PromptMouseMap). One per editor,
    // captured by both the CatchEvent below and the Renderer at the end.
    auto mouse_map = std::make_shared<PromptMouseMap>();

    // Accept the highlighted suggestion in place. Shared by Tab and (when the
    // list is open) Enter -- see below for why Enter needed this at all.
    // Completions may differ from the displayed text (a command completes to
    // its name plus a space, not to the annotation line).
    const auto accept_suggestion = [&] {
        if (state.suggestion_selected < static_cast<int>(state.completions.size())) {
            const std::string& completion = state.completions[state.suggestion_selected];
            if (IsCommandNameInProgress(state.content)) {
                // A command line IS its prefix: whole-line replacement.
                state.content = completion;
                state.cursor_position = static_cast<int>(state.content.size());
            } else {
                // A mention SPLICES the path over the @token: whole-line
                // replacement destroyed the user's prose AND manufactured
                // a '/'-leading line the dispatcher would route as a
                // command. The '@' stays -- the server's expansion scans
                // for '@' + non-space tokens.
                //
                // The token replaced is the one AT THE CURSOR (MentionTokenStart),
                // not content.rfind('@') over the whole buffer -- the old
                // whole-buffer search always found the LAST '@' typed, so
                // completing a mention the cursor had been moved back INTO
                // (to fix an earlier one, say) spliced over -- and destroyed
                // -- every char after the cursor up to and past that later
                // '@', including whatever the user had typed after it. The
                // WHOLE token is replaced (MentionTokenEnd), not just up to
                // the cursor: completing mid-token ("@src|extra", cursor
                // between them) should not leave "extra" glued onto the
                // inserted path.
                if (const auto at = MentionTokenStart(state.content, state.cursor_position)) {
                    const std::size_t end = MentionTokenEnd(state.content, *at);
                    const std::string spliced = "@" + completion;
                    state.content =
                        state.content.substr(0, *at) + spliced + state.content.substr(end);
                    state.cursor_position = static_cast<int>(*at + spliced.size());
                } else {
                    // No '@' relative to the cursor -- should not normally
                    // happen (MentionPrefixAt gated the query that produced
                    // this completion list), but degrade to appending rather
                    // than silently dropping a selected completion.
                    state.content += "@" + completion;
                    state.cursor_position = static_cast<int>(state.content.size());
                }
            }
        }
        state.list_open = false;
        state.suggestions.clear();
        state.completions.clear();
    };

    // Bracketed-paste mode (DECSET 2004) tracking -- see the handler's
    // comment. LIVES ON THE COMPONENT (a shared_ptr the handler owns a copy
    // of), not in PromptEditorState: it is input MODE, not per-tab content,
    // and the state is copied into snapshots. Caller-supplied so
    // HandleGlobalKey (keybinding_dispatch.cpp) can share it -- see the
    // `pasting` parameter's doc comment in the header. Null (every existing
    // test call site) falls back to a private flag, exactly as before.
    if (!pasting) pasting = std::make_shared<bool>(false);

    // accept_suggestion, on_text, on_command and command_catalog captured BY
    // VALUE (copies), not by reference: `[&]` here would bind references to
    // this factory's OWN PARAMETERS -- on_text/on_command/command_catalog are
    // by-value std::functions living in this stack frame, not the caller's --
    // and every one of those references dangles the moment MakePromptEditor
    // returns, exactly the "closures outlive the factory" trap
    // session_picker.cpp documents (accept_suggestion is the same trap one
    // level up: a local closure object, not a reference to caller state).
    // `state` stays by reference deliberately: it is a reference parameter
    // binding to an object the caller (RunTui) owns for the app's lifetime,
    // which outlives this component.
    auto with_events = CatchEvent(
        input, [&state, &hist, owned_history, accept_suggestion, on_text, on_command,
                command_catalog, pasting, mouse_map, newline_check](const Event& event) {
        // Bracketed paste (DECSET 2004): the terminal wraps a paste in
        // \x1b[200~ ... \x1b[201~ and every line break inside it arrives as
        // a plain Return, which used to submit each pasted line as its own
        // turn. FTXUI 7 delivers an unknown CSI sequence as Event::Special
        // with the raw bytes preserved (terminal_input_parser.cpp: ParseCSI's
        // default arm returns SPECIAL; Event::Special keeps input()), so the
        // markers are matchable here. While pasting, Return inserts a
        // newline (exactly as Alt+Enter does -- InsertNewlineAtCursor) and
        // the pasted characters pass through to the Input unchanged. A
        // terminal without 2004 support never delivers the markers and
        // behavior is exactly as it was before.
        if (event.input().find("\x1b[200~") != std::string::npos) {
            *pasting = true;
            return true;   // swallowed
        }
        if (event.input().find("\x1b[201~") != std::string::npos) {
            *pasting = false;
            return true;   // swallowed
        }
        // A pasted tab is content, not completion: Tab-accept below must not
        // replace the pasted text with whatever suggestion happened to show.
        if (*pasting && event == Event::Tab) {
            const auto at = static_cast<std::size_t>(
                std::clamp(state.cursor_position, 0, static_cast<int>(state.content.size())));
            state.content.insert(at, "\t");
            state.cursor_position = static_cast<int>(at) + 1;
            return true;
        }
        if (*pasting && event == Event::Return) {
            InsertNewlineAtCursor(state);
            return true;   // consumed
        }

        // Readline line-editing: ctrl+a/e/u/k, the bindings terminal users
        // reach for by reflex. Operate on the WHOLE buffer, not a visual
        // line, matching FTXUI's own Home/End (input.cpp: cursor_position() =
        // 0 / content->size()) -- ctrl+a/e are exactly those two keys under
        // another name, so a multi-line prompt behaves the same either way.
        // ctrl+w (delete word backward, also standard readline) is
        // deliberately NOT here: it is already the global tab.close binding
        // (keybindings.cpp). Global dispatch runs BEFORE this CatchEvent
        // (HandleGlobalKey wraps the editor), so a key a global action
        // claims never arrives here at all -- which is why no default
        // global binding may use ctrl+a/e/k/u.
        // Byte encoding matches key_event_parser.cpp's CtrlLetter exactly
        // (ctrl+<letter> is ASCII letter - 'a' + 1); duplicated rather than
        // shared because that helper is file-local to key_event_parser.cpp.
        static const Event kCtrlA = Event::Character(std::string(1, '\x01'));
        static const Event kCtrlE = Event::Character(std::string(1, '\x05'));
        static const Event kCtrlK = Event::Character(std::string(1, '\x0B'));
        static const Event kCtrlU = Event::Character(std::string(1, '\x15'));
        if (event == kCtrlA) {
            state.cursor_position = 0;
            return true;
        }
        if (event == kCtrlE) {
            state.cursor_position = static_cast<int>(state.content.size());
            return true;
        }
        if (event == kCtrlU || event == kCtrlK) {
            const auto pos = std::clamp<std::size_t>(
                static_cast<std::size_t>(state.cursor_position), 0, state.content.size());
            if (event == kCtrlU) {
                state.content.erase(0, pos);
                state.cursor_position = 0;
            } else {
                state.content.erase(pos);
            }
            return true;
        }

        // The "prompt.newline" action (or, absent a resolved keymap,
        // IsNewlineEvent's hardcoded default) inserts a newline -- see
        // newline_check's construction above and the header's NEWLINE
        // comment.
        if (newline_check(event)) {
            InsertNewlineAtCursor(state);
            return true;   // consumed
        }
        // Enter accepts the highlighted suggestion when the list is open,
        // the same as Tab, rather than submitting the raw (incomplete)
        // prefix as a turn/command -- Enter is the more natural "I've chosen
        // this" key, and submitting `/comp` (not a real command) when the
        // user was reaching for `/compact` from the open list was a silent
        // trap. Only when the list is CLOSED does Enter fall through to
        // submit, unchanged from before.
        //
        // EXCEPT when the input already equals the selected completion
        // (trimmed of the trailing space a command completion adds): the
        // user typed the whole command out by hand, so this Enter fell
        // through and hit the (now-identical) "accept" path, which just
        // reinserted the same text and consumed the keystroke -- typing
        // "/help" in full and pressing Enter silently did nothing, requiring
        // a second Enter to actually submit. Falling through to the ordinary
        // submit path below instead.
        if (state.list_open && !state.suggestions.empty() && event == Event::Return) {
            bool already_typed = false;
            if (state.suggestion_selected < static_cast<int>(state.completions.size())) {
                std::string completed = state.completions[state.suggestion_selected];
                while (!completed.empty() &&
                       std::isspace(static_cast<unsigned char>(completed.back()))) {
                    completed.pop_back();
                }
                if (IsCommandNameInProgress(state.content)) {
                    already_typed = (completed == state.content);
                } else if (const auto at =
                               MentionTokenStart(state.content, state.cursor_position)) {
                    // A mention's completion only replaces its OWN token
                    // (accept_suggestion splices over MentionTokenStart..End,
                    // not the whole line), so "already typed" must compare
                    // against that token -- "@" + completed -- not the whole
                    // buffer, which for a mention always also carries the
                    // surrounding prose and can never equal it.
                    const std::size_t end = MentionTokenEnd(state.content, *at);
                    const std::string token = state.content.substr(*at, end - *at);
                    already_typed = (token == "@" + completed);
                }
            }
            if (!already_typed) {
                accept_suggestion();
                return true;
            }
        }
        // Escape with the suggestion list open DISMISSES the list, rather
        // than falling through to whatever the raw Input does with the
        // keystroke: the list previously could not be put away without
        // editing the text (changing the prefix was the only close). Only
        // consumed while the list is open -- an idle Escape with the list
        // closed still reaches the global handler (turn.cancel and friends).
        if (state.list_open && event == Event::Escape) {
            state.list_open = false;
            state.suggestions.clear();
            state.completions.clear();
            state.suggestion_selected = 0;
            return true;   // consumed
        }
        if (event == Event::Return) {
            std::string line = state.content;
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
            if (line.empty()) return true;
            // Recall history (no arrow-up-to-recall was one of the editor's
            // named gaps). The timeline lives in PromptHistory -- per-tab
            // editors share ONE app-scoped instance (RunTui passes the same
            // pointer to every tab's editor), so up-arrow continuity holds
            // across tab switches; dedup and the 500-entry bound are
            // Record's. The browse position is per-tab (state.history_cursor,
            // not PromptHistory), so submitting always exits browsing for
            // THIS tab only.
            hist.Record(line);
            state.history_cursor.reset();
            state.undo.Clear();
            if (line.front() == '/') {
                on_command(line);
            } else {
                on_text(line);
            }
            state.content.clear();
            // The caret and any stashed history draft belong to the line
            // just sent: left set, the next line's editing started from a
            // stale position and ArrowDown restored the old draft.
            state.cursor_position = 0;
            state.history_draft.clear();
            state.list_open = false;
            state.suggestions.clear();
            state.completions.clear();
            return true;   // consumed
        }
        // The suggestion list claims Up/Down ONLY while the user is picking
        // from it -- not while a history browse is in progress
        // (state.history_cursor set): a browse must be walkable in both
        // directions even though a recalled `/cmd` line legitimately re-arms
        // the command list on the next render (the render gate rebuilds it
        // from the typed name). Browsing arrows are history's (the arms
        // below); the list itself stays visible and Tab still completes from
        // it.
        if (!state.history_cursor.has_value() && state.list_open && event == Event::ArrowDown &&
            !state.suggestions.empty()) {
            state.suggestion_selected =
                (state.suggestion_selected + 1) % static_cast<int>(state.suggestions.size());
            return true;
        }
        if (!state.history_cursor.has_value() && state.list_open && event == Event::ArrowUp &&
            !state.suggestions.empty()) {
            state.suggestion_selected = (state.suggestion_selected +
                                         static_cast<int>(state.suggestions.size()) - 1) %
                                        static_cast<int>(state.suggestions.size());
            return true;
        }
        if (state.list_open && event == Event::Tab && !state.suggestions.empty()) {
            accept_suggestion();
            return true;   // consumed
        }
        // Submitted-line recall (readline-like). HISTORY OWNS THE ARROW KEYS
        // WHILE BROWSING -- the guard is `!state.history_cursor.has_value()`,
        // not `!state.list_open`: a recalled `/cmd` line opens the command
        // suggestion list on the NEXT RENDER (IsCommandNameInProgress is
        // true; the render gate at the bottom of this file sets list_open),
        // and with the old guard every further ArrowUp/ArrowDown was eaten
        // by the list-navigation branches above -- history could not be
        // walked past the first '/'-leading entry ("history can't go up
        // once you get to the first / command"). While state.history_cursor
        // is set, the user is BROWSING, not picking a completion: Up/Down walk the
        // timeline, and a suggestion list armed on the recalled line is
        // stale by construction (it described the content before the
        // recall) -- dismissed here, on the same keypress. Any list the
        // recalled line genuinely deserves re-arms from the render gate on
        // the first edit (prefix change re-derives the match). Tab still
        // completes while browsing (its branch at the top is not
        // history-gated) -- completing a recalled line is a reasonable edit.
        // Single-line drafts only: a multi-line draft (built with Alt+Enter)
        // has its own use for Up -- moving the cursor up a line -- and
        // recalling history over it silently discarded the draft. Falling
        // through hands the event to the wrapped Input, which already moves
        // the cursor up a visual line for multi-line content.
        if (event == Event::ArrowUp && !hist.entries.empty() &&
            state.content.find('\n') == std::string::npos) {
            // Entering browse mode dismisses whatever suggestion list is
            // showing (see the guard comment above). Walking OLDER while
            // already browsing keeps going from the current position.
            if (!state.history_cursor.has_value()) {
                state.list_open = false;
                state.suggestions.clear();
                state.completions.clear();
                state.suggestion_selected = 0;
                state.history_draft = state.content;
                state.history_cursor = hist.entries.size() - 1;
            } else if (*state.history_cursor > 0) {
                --*state.history_cursor;
            }
            state.content = hist.entries[*state.history_cursor];
            // FTXUI's Input only CLAMPS cursor_position into content's new
            // bounds after an external splice -- it never MOVES it -- so
            // without this the caret stayed wherever it was in the PREVIOUS
            // line's text, landing mid-word (or past the end, silently
            // clamped) in the recalled one instead of at the end the way a
            // shell's history recall behaves.
            state.cursor_position = static_cast<int>(state.content.size());
            return true;
        }
        if (event == Event::ArrowDown && state.history_cursor.has_value()) {
            if (*state.history_cursor + 1 < hist.entries.size()) {
                ++*state.history_cursor;
                state.content = hist.entries[*state.history_cursor];
            } else {
                state.history_cursor.reset();
                state.content = state.history_draft;
            }
            state.cursor_position = static_cast<int>(state.content.size());
            return true;
        }

        // After every character edit, the @-mention match must re-derive --
        // the query itself fires from the render gate below -- no timers, no
        // threads; frame timing carries it. But the DERIVATION moved OUT of
        // this handler (along with the '/'+name command match, once
        // here too): this handler runs BEFORE the wrapped Input applies the
        // event (see ftxui's catch_event.cpp, quoted in the comment below),
        // so the state.content/cursor_position reads were one keystroke
        // stale -- after "@ab" the debounce queried "a", a Backspace queried
        // the prefix INCLUDING the char about to be deleted, and a bare "@"
        // never armed at all. Both matches now derive at render time (below
        // in the Renderer), reading content AFTER Input has applied every
        // event so far this frame. Only bookkeeping that reads nothing of
        // the buffer stays here.
        if (event.is_character() || event == Event::Backspace || event == Event::Delete) {
            // Editing a recalled line exits browsing -- it is no longer the
            // same text as history[cursor], so further up/down should not
            // silently discard what was just typed.
            state.history_cursor.reset();
        }

        // Click-to-position, re-implemented from the render pass's geometry
        // (mouse_map): the wrapped Input's own mouse handling relied on
        // reflected boxes inside ITS rendered element, which is no longer
        // what the frame draws (see the Renderer below). Row from the screen
        // y (the rendered window starts at mouse_map->first_row), byte offset
        // from the click column. Everything else (wheel, drag, clicks on the
        // suggestion list or elsewhere) falls through untouched.
        if (event.is_mouse()) {
            // Event::mouse() is not const in FTXUI 7, so take a copy of the
            // (plain-value) event rather than const_cast.
            Event mutable_event = event;
            const auto& mouse = mutable_event.mouse();
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed &&
                mouse_map->box.Contain(mouse.x, mouse.y) && !mouse_map->wrap.lines.empty()) {
                const std::size_t row_index =
                    static_cast<std::size_t>(mouse.y - mouse_map->box.y_min) +
                    mouse_map->first_row;
                if (row_index < mouse_map->wrap.lines.size()) {
                    const PromptVisualLine& row = mouse_map->wrap.lines[row_index];
                    const auto target = static_cast<std::size_t>(
                        std::max(mouse.x - mouse_map->box.x_min - kPromptGutter, 0));
                    // Largest byte offset whose column position fits before
                    // the click column, never splitting a code point: a wide
                    // glyph clicked on its first column selects the glyph, on
                    // its second the position stays at its start.
                    std::size_t off = 0;
                    std::size_t col = 0;
                    std::size_t best = 0;
                    while (off < row.text.size()) {
                        const std::size_t cp_cols =
                            Utf8CharColumnWidth(Utf8Decode(row.text, off));
                        if (col + cp_cols > target) break;
                        col += cp_cols;
                        best = off;
                    }
                    state.cursor_position =
                        static_cast<int>(row.begin + best);
                }
                return true;   // a press inside the prompt body is ours
            }
        }
        return false;   // everything else goes to the Input
    });

    auto undoable = Make<UndoableEditor>(with_events, state, pasting, key_matches);

    return Renderer(undoable,
                    [=, &state, &theme, cached_command_prefix = std::string(),
                     cached_command_session_ok = false,
                     cached_command_subagent = false,
                     cached_command_suggestions = std::vector<std::string>(),
                     cached_command_catalog =
                         std::shared_ptr<const std::vector<CommandCatalogEntry>>(),
                     has_cached_command = false, cached_mention_prefix = std::string(),
                     has_cached_mention = false]() mutable {
        using namespace ftxui;

        // The '/'+name command catalog match: read HERE, after Input has
        // applied every event so far this frame, not from the character
        // handler above (see its comment for why that read was stale).
        // IsCommandNameInProgress, not "prefix non-empty": a bare "/" has an
        // EMPTY prefix (CommandPrefixOf("/") == ""), and that must still
        // mean "list every command" (rfind("", 0) == 0 matches any name),
        // not "not a command line".
        if (IsCommandNameInProgress(state.content) && command_catalog != nullptr) {
            const std::string command_prefix = CommandPrefixOf(state.content);
            // The focused tab's session presence, probed once per frame this
            // runs: entries marked requires_session are offered only when a
            // session exists (a sessionless tab never offers a command that
            // would only refuse). The subagent probe likewise: on a child
            // tab only UsableOnSubagent entries are offered, because the
            // server refuses everything else there.
            const bool session_ok = !session_available || session_available();
            const bool subagent_only = subagent_read_only && subagent_read_only();

            // Skip the catalog scan (and its per-entry string concatenation)
            // when nothing that could change the result has: same prefix,
            // same session availability, AND state.suggestions still holds
            // exactly what THIS closure last wrote there. That third check
            // is what makes this safe rather than merely fast -- it
            // self-heals against an external clear (accept_suggestion's
            // Tab/Enter handling, the character handler's re-arm) without
            // this closure needing to know about either: a mismatch just
            // falls through to the full rebuild below, same as a cache miss.
            // This runs every frame (~20 Hz) for as long as a command name
            // is being typed; the catalog is small and fixed since connect,
            // but re-walking it and re-concatenating every entry's display
            // string on every frame was still pure waste whenever nothing
            // had actually changed.
            // The catalog snapshot too: a background refresh (a command file
            // added to the project) replaces it, and the pointer's identity
            // is exactly "did it change". Held here, so it cannot be freed
            // and its address reused while this closure still compares it.
            const auto catalog = command_catalog();
            const bool cache_hit = has_cached_command && catalog == cached_command_catalog &&
                                   command_prefix == cached_command_prefix &&
                                   session_ok == cached_command_session_ok &&
                                   subagent_only == cached_command_subagent &&
                                   state.suggestions == cached_command_suggestions;
            if (!cache_hit) {
                std::vector<std::string> suggestions;
                std::vector<std::string> completions;
                for (const auto& entry : *catalog) {
                    // The sessionless gate (see UsableWithoutSession): a
                    // session-bound command is offered only while a session
                    // exists; /agent and /model always are (their forms
                    // never refuse for want of one).
                    if (!UsableWithoutSession(entry) && !session_ok) continue;
                    // The subagent gate (see UsableOnSubagent): a child's
                    // command surface is read-only server-side, so a
                    // mutating entry is never offered where it would only
                    // be refused.
                    if (subagent_only && !UsableOnSubagent(entry)) continue;
                    if (entry.name.rfind(command_prefix, 0) != 0) continue;
                    std::string display = "/" + entry.name + "  ·  " + entry.summary;
                    if (entry.submits_prompt) display += "  (submits a prompt as your turn)";
                    if (entry.read_only) display += "  (read-only)";
                    suggestions.push_back(std::move(display));
                    completions.push_back("/" + entry.name + " ");
                }
                // Only touch the selection when the matched SET actually
                // changed -- resetting suggestion_selected unconditionally
                // would undo an ArrowUp/ArrowDown the user just made while
                // the prefix stayed put.
                if (suggestions != state.suggestions) {
                    state.suggestions = std::move(suggestions);
                    state.completions = std::move(completions);
                    state.suggestion_selected = 0;
                }
                has_cached_command = true;
                cached_command_catalog = catalog;
                cached_command_prefix = command_prefix;
                cached_command_session_ok = session_ok;
                cached_command_subagent = subagent_only;
                cached_command_suggestions = state.suggestions;
            }
            state.list_open = !state.suggestions.empty();
            state.query_pending = false;
            state.query_pending_hint.store(false, std::memory_order_release);
            // Left mention mode for command mode: nothing to remember, so a
            // return to a mention token re-arms from scratch (otherwise a
            // re-entered token with an unchanged prefix would look cached
            // and never query again).
            has_cached_mention = false;
        } else {
            has_cached_command = false;   // left command-name mode: nothing to remember
        }

        // The '@'+name mention match: derived HERE like the command catalog
        // above, reading state.content AFTER Input has applied every event
        // so far this frame (the old arming ran in the character handler and
        // was one keystroke stale for the same reason -- see that handler's
        // comment). Anchored at the cursor, so it tracks the token the
        // cursor sits in RIGHT NOW. A bare "@" is a live query too: its
        // empty prefix resolves immediately to an empty answer with no RPC
        // (app.cpp's `suggest` lambda), so arming it is free and the empty
        // answer simply keeps the list closed.
        if (!IsCommandNameInProgress(state.content)) {
            const bool in_token =
                MentionTokenStart(state.content, state.cursor_position).has_value();
            const std::string prefix =
                in_token ? MentionPrefixAt(state.content, state.cursor_position) : std::string();
            // No live token means nothing may stay completable: vectors held
            // over from the command path (the "/se" then three-backspaces
            // regression) or from an answer that landed after its token died
            // must go, or the next token change could splice a stale entry.
            // The empty-check makes this a no-op on every steady-state frame.
            if (!in_token && !state.suggestions.empty()) {
                state.suggestions.clear();
                state.completions.clear();
                state.suggestion_selected = 0;
                state.query_pending = false;
                state.query_pending_hint.store(false, std::memory_order_release);
            }
            if (in_token != has_cached_mention || prefix != cached_mention_prefix) {
                // The token appeared, left, or the typed prefix moved past
                // the cached one: the old list matches text that is no
                // longer under the cursor, and Tab within the debounce
                // window would complete the STALE entry. Clear it and re-arm
                // the debounce -- what the character handler used to do on
                // every edit. The list re-opens when the fresh answer lands.
                state.suggestions.clear();
                state.completions.clear();
                state.suggestion_selected = 0;
                state.query_at = std::chrono::steady_clock::now() + kSuggestDebounce;
                state.pending_prefix = prefix;
                state.query_pending = in_token;
                // Mirrored for the redraw pump (RunTui, a different thread) --
                // see PromptEditorState::query_pending_hint.
                state.query_pending_hint.store(in_token, std::memory_order_release);
                has_cached_mention = in_token;
                cached_mention_prefix = prefix;
            }
            state.list_open = in_token && !state.suggestions.empty();
        }

        // The debounce gate: once the delay has passed and the prefix is
        // unchanged, ask the server. suggest() returning nullopt means the
        // async fetch is still in flight (T2) -- keep polling on later
        // frames rather than treating "not yet" as "empty"; a changed
        // prefix re-arms pending_prefix/query_at in the mention match above
        // and this check
        // naturally moves on to the newer prefix, which is what makes a
        // stale answer harmless once it eventually arrives (see
        // AppState::RequestSuggestPaths / the `suggest` lambda in app.cpp).
        // A resolved-but-empty answer closes the list, same as before.
        if (state.query_pending && std::chrono::steady_clock::now() >= state.query_at) {
            if (auto result = suggest(state.pending_prefix)) {
                state.suggestions = std::move(*result);
                state.completions = state.suggestions;
                state.query_pending = false;
                state.query_pending_hint.store(false, std::memory_order_release);
                state.list_open = !state.suggestions.empty();
            }
        }

        Elements lines;
        lines.push_back(separator() | color(theme.ColorFor("border_unfocused")));

        // The prompt body: SOFT-WRAPPED. FTXUI's Input renders one terminal
        // row per LOGICAL line (a split at '\n' and nothing else), so a long
        // single-line prompt scrolled horizontally and could not be reviewed
        // before sending. The editor therefore renders its own DOM from
        // WrapPromptContent's visual lines; the wrapped Input stays in the
        // component tree (it still owns every key event) -- only its Render()
        // output is replaced, which is also why click-to-position had to move
        // into the CatchEvent above.
        const ftxui::Dimensions term = ftxui::Terminal::Size();
        const PromptWrapResult wrap = WrapPromptContent(
            state.content, static_cast<std::size_t>(std::max(state.cursor_position, 0)),
            static_cast<std::size_t>(std::max(term.dimx - kPromptGutter, 0)));

        // Visible window over the wrapped rows: at most half the screen (min
        // 4), anchored so the caret row is always inside it. Stateless on
        // purpose: the content only changes by editing, and editing moves the
        // caret with it, so re-anchoring on the caret row each frame is the
        // whole scroll model -- there is nothing to look at without also
        // moving the caret there.
        constexpr std::size_t kMinPromptRows = 4;
        const std::size_t cap =
            std::max(kMinPromptRows, static_cast<std::size_t>(std::max(term.dimy, 0)) / 2);
        const std::size_t first_row = wrap.cursor_row >= cap ? wrap.cursor_row - cap + 1 : 0;
        const std::size_t last_row = std::min(wrap.lines.size(), first_row + cap);

        const ftxui::Decorator caret_deco =
            input->Focused() ? ftxui::Decorator(ftxui::focusCursorBarBlinking)
                             : ftxui::Decorator(ftxui::focus);

        // text() draws a tab as nothing, while the wrap math counts it as
        // one column (Utf8CharColumnWidth): shown as one space, the caret
        // and click mapping line up. Display only -- the buffer keeps the tab.
        const auto shown = [](std::string row_text) {
            std::replace(row_text.begin(), row_text.end(), '\t', ' ');
            return ftxui::text(std::move(row_text));
        };
        Elements body;
        if (state.content.empty()) {
            // Placeholder, decorated the way FTXUI's own Input renders one:
            // dim text with the caret parked at the row start.
            body.push_back(text(kPromptPlaceholder) | Muted(theme) | caret_deco);
        } else {
            const auto cursor_byte = static_cast<std::size_t>(
                std::clamp(state.cursor_position, 0, static_cast<int>(state.content.size())));
            for (std::size_t k = first_row; k < last_row; ++k) {
                const PromptVisualLine& row = wrap.lines[k];
                if (k != wrap.cursor_row) {
                    body.push_back(shown(row.text));
                    continue;
                }
                // The caret row: split at the cursor byte so the glyph under
                // it carries the terminal cursor. focusCursor* both shapes
                // the cursor and anchors any enclosing frame's scroll --
                // exactly how FTXUI's own Input decorates its caret cell.
                const std::size_t offset = cursor_byte - row.begin;
                std::size_t glyph_end = offset;
                if (glyph_end < row.text.size()) {
                    Utf8Decode(row.text, glyph_end);
                }
                const std::string glyph =
                    offset < row.text.size() ? row.text.substr(offset, glyph_end - offset)
                                             : std::string(" ");   // caret past the last glyph
                body.push_back(hbox({shown(row.text.substr(0, offset)),
                                     shown(glyph) | caret_deco,
                                     shown(row.text.substr(glyph_end))}));
            }
        }
        // The gutter: "❯ " marks where input goes (on the first logical row
        // only -- a scrolled window shows blanks), two spaces keep the
        // wrapped rows aligned under it.
        for (std::size_t k = 0; k < body.size(); ++k) {
            const bool first_line = k == 0 && first_row == 0;
            body[k] = hbox({text(first_line ? "❯ " : "  ") |
                                color(theme.ColorFor("border_focused")) | bold,
                            std::move(body[k])});
        }
        // Publish the geometry the mouse handler maps clicks against (see
        // PromptMouseMap): the rendered vbox is exactly `last_row -
        // first_row` tall starting at first_row, so y -> row is arithmetic
        // (and x is offset by the gutter).
        mouse_map->wrap = wrap;
        mouse_map->first_row = first_row;
        // Body text role, not the terminal's own foreground.
        lines.push_back(vbox(std::move(body)) | color(theme.ColorFor("assistant_message")) |
                        reflect(mouse_map->box));

        if (state.list_open) {
            Elements items;
            for (int i = 0; i < static_cast<int>(state.suggestions.size()); ++i) {
                const bool selected = i == state.suggestion_selected;
                auto item = text("  " + state.suggestions[i]);
                item |= color(selected ? theme.ColorFor("border_focused")
                                       : theme.ColorFor("system_message"));
                if (selected) {
                    item |= bold;
                    // The DOM scroll anchor (not keyboard focus -- see
                    // ChatView's row cursor for the same pattern): keeps the
                    // selected row inside the capped, scrollable viewport
                    // below as Up/Down moves past its edge.
                    item |= focus;
                }
                items.push_back(std::move(item));
            }
            // Capped and scrollable: an unbounded list (a broad "/" or "@"
            // prefix against a large catalog) used to grow as tall as it
            // needed, squeezing the transcript above it out of view.
            //
            // Decorator ORDER is load-bearing (FTXUI 7): `yframe` must wrap
            // the content and `size` must wrap the FRAME. The reverse --
            // size | yframe -- broke scrolling entirely: Size::SetBox clamps
            // the content's box to the cap, but the frame's own requirement
            // min_y shrinks to the cap too, so when the surrounding layout
            // hands the frame MORE than the cap the frame sees
            // "internal == external, nothing to scroll" (dy clamps to 0) and
            // the content below the cap is simply CLIPPED -- arrowing down
            // past the edge stopped moving the view. yframe-first keeps the
            // full content height inside the frame, so the `focus` anchor on
            // the selected row actually scrolls it into view.
            lines.push_back(vbox(std::move(items)) | yframe |
                            size(HEIGHT, LESS_THAN, 8) | vscroll_indicator);
        }
        return vbox(std::move(lines));
    });
}

// The per-tab editor host. A ComponentBase subclass, NOT a
// Renderer+CatchEvent composition -- for the reason modal_host.cpp
// documents at length: a plain composition never registers the swapped-in
// child as a real ftxui child, so Focusable()/ActiveChild() fall back to
// ComponentBase's defaults (no children), Focused() is false along the whole
// container chain above, and every keystroke is dropped before it can reach
// the editor. Overriding the pair to defer to the LIVE slot is what keeps
// the host (and therefore the editor, and therefore the Input) visible to
// ftxui's own focus machinery.
class PerTabEditorHost : public ftxui::ComponentBase {
public:
    PerTabEditorHost(PerTabPromptEditors& tabs, std::function<std::string()> focused_tab,
                     std::function<ftxui::Component(PromptEditorState&)> build_editor)
        : tabs_(tabs), focused_tab_(std::move(focused_tab)), build_editor_(std::move(build_editor)) {}

private:
    ftxui::Component resolve() { return tabs_.Resolve(focused_tab_(), build_editor_); }

    ftxui::Element OnRender() override {
        ftxui::Component editor = resolve();
        // emptyElement (not a default-constructed Element): the frame always
        // reserves the editor's row (kCriticalHeight), so the no-editor
        // frame must render an empty node, not hand ftxui a null pointer.
        return editor ? editor->Render() : ftxui::emptyElement();
    }

    bool OnEvent(ftxui::Event event) override {
        // Resolved LIVE: a key arriving in the same frame as a tab switch
        // lands in the tab focused at event time -- the same rule the submit
        // paths follow via focused_or_new_tab. Unhandled events return false
        // and keep propagating up (to with_keys' HandleGlobalKey and the
        // outer containers), the same posture as any leaf component.
        ftxui::Component editor = resolve();
        return editor != nullptr && editor->OnEvent(std::move(event));
    }

    // The ModalHost contract: being the focused tab's editor IS being the
    // active thing. Both defer to the live slot, so Focused()'s walk from
    // the root (through the modal stack, with_keys, session_view) terminates
    // in the Input that should hold the keyboard.
    bool Focusable() const override {
        std::lock_guard lock(tabs_.mtx_);
        const auto it = tabs_.editors_.find(focused_tab_());
        return it != tabs_.editors_.end() && it->second != nullptr;
    }

    ftxui::Component ActiveChild() override { return resolve(); }

    PerTabPromptEditors& tabs_;
    std::function<std::string()> focused_tab_;
    std::function<ftxui::Component(PromptEditorState&)> build_editor_;
};

ftxui::Component MakePerTabPromptEditor(
    PerTabPromptEditors& tabs, std::function<std::string()> focused_tab,
    std::function<ftxui::Component(PromptEditorState&)> build_editor) {
    return Make<PerTabEditorHost>(tabs, std::move(focused_tab), std::move(build_editor));
}

// --- PerTabPromptEditors -----------------------------------------------------

ftxui::Component PerTabPromptEditors::Resolve(
    const std::string& tab,
    const std::function<ftxui::Component(PromptEditorState&)>& build) {
    std::lock_guard lock(mtx_);
    auto& slot = editors_[tab];
    if (slot == nullptr) {
        // build() runs under the mutex: build_editor_ captures RunTui locals
        // by reference and only ever runs on the FTXUI loop thread (this and
        // OnEvent), so the lock is uncontended in practice -- it exists for
        // the pump's QueryPendingHint, not for reentrancy.
        slot = build(states_[tab]);
    }
    return slot;
}

PromptEditorState& PerTabPromptEditors::CurrentState(const std::string& tab) {
    std::lock_guard lock(mtx_);
    return states_[tab];
}

bool PerTabPromptEditors::QueryPendingHint(const std::string& tab) const {
    std::lock_guard lock(mtx_);
    const auto it = states_.find(tab);
    // find, not operator[]: a tab the pump peeked at must not have a state
    // minted for it from another thread; absent means "nothing armed".
    return it != states_.end() && it->second.query_pending_hint.load(std::memory_order_acquire);
}

void PerTabPromptEditors::ForgetTab(const std::string& tab) {
    std::lock_guard lock(mtx_);
    // The editor dies WITH its state: its closures captured the state's
    // address, so erasing one without the other is the exact dangling bug
    // class filterable_menu's lifetime guards document. The "" (pre-tab)
    // entry is deliberately never forgotten -- it IS the app's no-tab editor.
    if (!tab.empty()) {
        states_.erase(tab);
        editors_.erase(tab);
    }
}

// --- MentionQuerySlots -------------------------------------------------------

std::optional<std::vector<std::string>> MentionQuerySlots::Poll(const std::string& tab,
                                                                const std::string& prefix) {
    std::lock_guard lock(mtx_);
    Slot& slot = slots_[tab];
    if (slot.prefix == prefix) {
        if (slot.ready) {
            // Consume: this answer belongs to exactly the query the caller
            // just asked about, and the MentionSuggestor contract is
            // deliver-once (the render gate stops polling after a present
            // result).
            slot.ready = false;
            slot.in_flight = false;
            return std::move(slot.result);
        }
        if (slot.in_flight) return std::nullopt;   // still waiting; poll again next frame
    }
    // A new (tab, prefix) pair: arm it. Whatever was in flight for this tab
    // under a different prefix is superseded -- its completion, if it arrives
    // late, fails Complete's prefix guard below and is dropped rather than
    // clobbering the newer query. An answer that landed but was never
    // consumed (the caller re-armed before the next frame) is discarded the
    // same way: ready/result are overwritten here.
    slot.prefix = prefix;
    slot.in_flight = true;
    slot.ready = false;
    slot.result.clear();
    return std::nullopt;
}

void MentionQuerySlots::Complete(const std::string& tab, const std::string& prefix,
                                 std::vector<std::string> paths) {
    std::lock_guard lock(mtx_);
    const auto it = slots_.find(tab);
    // No slot: the query died with the tab (ForgetTab) -- nothing to deliver.
    // A different prefix: the worker's answer is for a query the user has
    // already moved past -- the staleness drop the shared-slot design could
    // not make per-tab, which is what let one tab's answer poison another's.
    if (it == slots_.end() || it->second.prefix != prefix) return;
    it->second.result = std::move(paths);
    it->second.ready = true;
    it->second.in_flight = false;
}

void MentionQuerySlots::Forget(const std::string& tab) {
    std::lock_guard lock(mtx_);
    slots_.erase(tab);
}

// --- MakeMentionSuggestor ----------------------------------------------------

MentionSuggestor MakeMentionSuggestor(
    AppState* app, MentionQuerySlots& slots,
    std::function<std::string(const std::string& session_id)> session_root_fallback) {
    // `session_root_fallback` maps a session id to the project root a
    // SESSIONLESS-flavored query would use. Production passes the empty
    // function (sessioned tabs query BY SESSION -- the server resolves the
    // root); tests pass a table so no RPC worker is needed. Kept a parameter
    // rather than a AppState method probe because the fallback's correct
    // answer differs per caller and the seam documents that in one place.
    return [app, &slots, session_root_fallback = std::move(session_root_fallback)](
               const std::string& prefix) -> std::optional<std::vector<std::string>> {
        if (app == nullptr) return std::vector<std::string>{};
        const std::string tab = app->FocusedTabId();
        const std::string sid = app->FocusedSessionId();

        // Per-tab slot, keyed by the tab id the scope used to embed. The
        // slot handoff (nullopt = armed-and-waiting) comes FIRST: the
        // subscription is what makes a later Complete() deliverable, so it
        // must be registered before the RPC can answer, whatever the
        // submission path below does.
        auto armed = slots.Poll(tab, prefix);
        if (armed.has_value()) return armed;   // consume the resolved answer

        // Submission. Empty prefix INCLUDED: a bare `@` is a live query whose
        // empty prefix asks the server for the project-root listing (the
        // resolver answers "" with the root's entries, directories
        // '/'-suffixed -- bash-completion shape). The old client-side
        // `if (prefix.empty()) return {}` short-circuit made a bare `@` a
        // permanent no-op -- the list could never open. The server scopes
        // every request (session id or project root, validated & clamped
        // server-side), so nothing here is unscoped even for "".
        if (!sid.empty()) {
            app->RequestSuggestPaths(sid, prefix, 20, [&slots, tab, prefix](
                                                          std::vector<std::string> paths) {
                slots.Complete(tab, prefix, std::move(paths));
            });
        } else {
            // Sessionless tab: scope to the project root the tab's first
            // message would create the session with. Production passes
            // CurrentProjectRootOrEmpty(); a test's fallback answers from
            // its table.
            const std::string root =
                session_root_fallback ? session_root_fallback(sid) : std::string();
            if (root.empty()) {
                // No project context at all (a stray cwd failure): the
                // historical empty answer. Complete() is deliberately NOT
                // called: Poll armed the slot, so feeding it an empty answer
                // would open-then-close the list on the next frame; leaving
                // it in-flight keeps the armed state honest (nothing is
                // coming, but nothing was asked either) and the next prefix
                // change re-arms cleanly.
                return std::nullopt;
            }
            app->RequestSuggestPathsForRoot(root, prefix, 20, [&slots, tab, prefix](
                                                                  std::vector<std::string> paths) {
                slots.Complete(tab, prefix, std::move(paths));
            });
        }
        return std::nullopt;
    };
}

}  // namespace minimal::client
