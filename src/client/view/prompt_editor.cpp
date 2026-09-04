#include "minimal/client/view/prompt_editor.hpp"

#include <algorithm>

#include <ftxui/component/event.hpp>

namespace minimal::client {

namespace {

using ftxui::Element;
using ftxui::Event;

constexpr auto kSuggestDebounce = std::chrono::milliseconds(120);

// The token being completed: the text after the last '@' not yet closed by
// whitespace. Bash-completion shape: a mention is '@' + non-space characters.
std::string MentionPrefixAt(const std::string& text) {
    const std::size_t at = text.rfind('@');
    if (at == std::string::npos) return "";
    for (std::size_t i = at + 1; i < text.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) return "";
    }
    return text.substr(at + 1);
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

}  // namespace

ftxui::Component MakePromptEditor(
    const Theme& theme, PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command,
    const MentionSuggestor& suggest,
    CommandCatalogSource command_catalog,
    std::function<bool()> session_available) {
    using namespace ftxui;

    // Multiline ON: Return-as-newline is the Input's default, so the submit
    // decision is ours -- CatchEvent sees every event first. cursor_position
    // is bound explicitly (not the 2-arg factory) so accept_suggestion below
    // can move the caret after splicing in a completion -- FTXUI's own
    // InputOption otherwise owns that position privately, unreachable from
    // here, and only clamps it into range rather than moving it.
    InputOption input_option;
    input_option.content = &state.content;
    input_option.placeholder = "type here; @ for files, / for commands, alt+enter for newline";
    input_option.cursor_position = &state.cursor_position;
    auto input = Input(input_option);

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
                const std::size_t at = state.content.rfind('@');
                if (at != std::string::npos) {
                    state.content = state.content.substr(0, at) + "@" + completion;
                } else {
                    state.content += "@" + completion;
                }
                // MentionPrefixAt only offers a mention when the '@' token
                // runs to the end of content with no trailing whitespace, so
                // the splice above always lands the completion at the end.
                state.cursor_position = static_cast<int>(state.content.size());
            }
        }
        state.list_open = false;
        state.suggestions.clear();
        state.completions.clear();
    };

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
        input, [&state, accept_suggestion, on_text, on_command,
                command_catalog](const Event& event) {
        // Readline line-editing: ctrl+a/e/u/k, the bindings terminal users
        // reach for by reflex. Operate on the WHOLE buffer, not a visual
        // line, matching FTXUI's own Home/End (input.cpp: cursor_position() =
        // 0 / content->size()) -- ctrl+a/e are exactly those two keys under
        // another name, so a multi-line prompt behaves the same either way.
        // ctrl+w (delete word backward, also standard readline) is
        // deliberately NOT here: it is already the global tab.close binding
        // (keybindings.cpp), and the editor's CatchEvent runs before that
        // dispatch -- claiming it here would silently break closing a tab
        // while the prompt has focus, which is effectively always.
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

        // Alt+Enter (ESC CR) inserts a newline -- the portable multi-line key.
        static const Event kAltEnterCr = Event::Character("\x1b\r");
        static const Event kAltEnterLf = Event::Character("\x1b\n");
        if (event == kAltEnterCr || event == kAltEnterLf) {
            // AT THE CURSOR, not appended to the end: appending always broke
            // a newline inserted while editing mid-line (the common case --
            // Alt+Enter is how a multi-line prompt gets built up one line at
            // a time) into a line dropped at the tail instead. cursor_position
            // is clamped defensively -- FTXUI only clamps it INTO content's
            // bounds after an external splice, it never derives it, so a
            // stale value from before some other edit must not be trusted
            // past content's own length.
            const auto pos = std::clamp<std::size_t>(
                static_cast<std::size_t>(state.cursor_position), 0, state.content.size());
            state.content.insert(pos, "\n");
            state.cursor_position = static_cast<int>(pos) + 1;
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
                already_typed = (completed == state.content);
            }
            if (!already_typed) {
                accept_suggestion();
                return true;
            }
        }
        if (event == Event::Return) {
            std::string line = state.content;
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
            if (line.empty()) return true;
            // Recall history (no arrow-up-to-recall was one of the editor's
            // named gaps): skip a consecutive duplicate of the last entry --
            // repeatedly submitting the same line, or repeating what recall
            // just brought back, should not grow the list every time.
            if (state.history.empty() || state.history.back() != line) {
                state.history.push_back(line);
            }
            state.history_cursor.reset();
            if (line.front() == '/') {
                on_command(line);
            } else {
                on_text(line);
            }
            state.content.clear();
            state.list_open = false;
            state.suggestions.clear();
            state.completions.clear();
            return true;   // consumed
        }
        if (state.list_open && event == Event::ArrowDown &&
            !state.suggestions.empty()) {
            state.suggestion_selected =
                (state.suggestion_selected + 1) % static_cast<int>(state.suggestions.size());
            return true;
        }
        if (state.list_open && event == Event::ArrowUp && !state.suggestions.empty()) {
            state.suggestion_selected = (state.suggestion_selected +
                                         static_cast<int>(state.suggestions.size()) - 1) %
                                        static_cast<int>(state.suggestions.size());
            return true;
        }
        if (state.list_open && event == Event::Tab && !state.suggestions.empty()) {
            accept_suggestion();
            return true;   // consumed
        }
        // Submitted-line recall (readline-like), only when the suggestion
        // list isn't already claiming these keys. ArrowUp walks toward
        // older entries; ArrowDown walks back toward newer ones and, past
        // the newest, restores whatever was being typed before recall
        // started.
        if (!state.list_open && event == Event::ArrowUp && !state.history.empty()) {
            if (!state.history_cursor.has_value()) {
                state.history_draft = state.content;
                state.history_cursor = state.history.size() - 1;
            } else if (*state.history_cursor > 0) {
                --*state.history_cursor;
            }
            state.content = state.history[*state.history_cursor];
            // FTXUI's Input only CLAMPS cursor_position into content's new
            // bounds after an external splice -- it never MOVES it -- so
            // without this the caret stayed wherever it was in the PREVIOUS
            // line's text, landing mid-word (or past the end, silently
            // clamped) in the recalled one instead of at the end the way a
            // shell's history recall behaves.
            state.cursor_position = static_cast<int>(state.content.size());
            return true;
        }
        if (!state.list_open && event == Event::ArrowDown && state.history_cursor.has_value()) {
            if (*state.history_cursor + 1 < state.history.size()) {
                ++*state.history_cursor;
                state.content = state.history[*state.history_cursor];
            } else {
                state.history_cursor.reset();
                state.content = state.history_draft;
            }
            state.cursor_position = static_cast<int>(state.content.size());
            return true;
        }

        // After every character edit, the @-mention debounce re-arms (the
        // query itself fires from the render gate below -- no timers, no
        // threads; frame timing carries it).
        //
        // Backspace/Delete re-arm too: they are edits, and leaving them out
        // stranded the OPEN list on deleted text -- "/se" then three
        // backspaces left the "/se" suggestions on screen over an EMPTY
        // input, where Enter "accepted" a completion the user had already
        // erased. The render-time command block re-populates the list the
        // same frame when the remaining text is still a command line, so
        // clearing here never flickers a live list off.
        //
        // The '/'+name command catalog match used to run HERE too, reading
        // state.content -- but CatchEventBase runs this handler BEFORE
        // passing the event to the wrapped Input (see ftxui's
        // catch_event.cpp: `if (on_event_(event)) return true; else return
        // ComponentBase::OnEvent(event);`), so the character just typed
        // had NOT been inserted into state.content yet. The match was
        // therefore always one keystroke stale -- invisible for a multi-
        // character prefix (the NEXT keystroke's check caught up), but total
        // silence for a BARE "/": there is no next keystroke to catch it up,
        // so the single biggest discoverability affordance in the app
        // showed nothing until a second character landed. It now runs at
        // render time instead (below, in the Renderer), reading content
        // AFTER Input has applied every event so far this frame.
        if (event.is_character() || event == Event::Backspace || event == Event::Delete) {
            // Editing a recalled line exits browsing -- it is no longer the
            // same text as history[cursor], so further up/down should not
            // silently discard what was just typed.
            state.history_cursor.reset();
            state.pending_prefix = MentionPrefixAt(state.content);
            state.query_pending = !state.pending_prefix.empty();
            // Mirrored for the redraw pump (RunTui, a different thread) --
            // see PromptEditorState::query_pending_hint.
            state.query_pending_hint.store(state.query_pending, std::memory_order_release);
            state.query_at = std::chrono::steady_clock::now() + kSuggestDebounce;
            state.list_open = state.query_pending;
            state.suggestion_selected = 0;
            // The OLD list must not survive the re-arm: it matches a prefix
            // the user has already typed past, and Tab within the debounce
            // window completed the STALE entry. The list re-opens when the
            // fresh answer lands.
            state.suggestions.clear();
            state.completions.clear();
        }
        return false;   // everything else goes to the Input
    });

    return Renderer(with_events, [=, &state, &theme] {
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
            // would only refuse).
            const bool session_ok = !session_available || session_available();
            std::vector<std::string> suggestions;
            std::vector<std::string> completions;
            for (const auto& entry : command_catalog()) {
                if (entry.requires_session && !session_ok) continue;
                if (entry.name.rfind(command_prefix, 0) != 0) continue;
                std::string display = "/" + entry.name + " -- " + entry.summary;
                if (entry.submits_prompt) display += "  (submits a prompt as your turn)";
                if (entry.read_only) display += "  (read-only)";
                suggestions.push_back(std::move(display));
                completions.push_back("/" + entry.name + " ");
            }
            // Only touch the selection when the matched SET actually
            // changed -- this runs every frame (~20 Hz), and resetting
            // suggestion_selected unconditionally would undo an ArrowUp/
            // ArrowDown the user just made while the prefix stayed put.
            if (suggestions != state.suggestions) {
                state.suggestions = std::move(suggestions);
                state.completions = std::move(completions);
                state.suggestion_selected = 0;
            }
            state.list_open = !state.suggestions.empty();
            state.query_pending = false;
            state.query_pending_hint.store(false, std::memory_order_release);
        }

        // The debounce gate: once the delay has passed and the prefix is
        // unchanged, ask the server. suggest() returning nullopt means the
        // async fetch is still in flight (T2) -- keep polling on later
        // frames rather than treating "not yet" as "empty"; a fresh
        // character re-arms pending_prefix/query_at above and this check
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
        lines.push_back(separator());
        lines.push_back(with_events->Render());

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

}  // namespace minimal::client
