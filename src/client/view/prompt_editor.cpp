#include "minimal/client/view/prompt_editor.hpp"

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

// The command name still being typed: content IS '/' + non-space characters,
// and nothing else. Once whitespace lands the command is chosen and editing
// its arguments is nobody's autocomplete business.
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
    std::function<std::vector<CommandCatalogEntry>()> command_catalog) {
    using namespace ftxui;

    // Multiline ON: Return-as-newline is the Input's default, so the submit
    // decision is ours -- CatchEvent sees every event first.
    auto input = Input(&state.content,
                       "type here; @ for files, / for commands, alt+enter for newline");

    // Accept the highlighted suggestion in place. Shared by Tab and (when the
    // list is open) Enter -- see below for why Enter needed this at all.
    // Completions may differ from the displayed text (a command completes to
    // its name plus a space, not to the annotation line).
    const auto accept_suggestion = [&] {
        if (state.suggestion_selected < static_cast<int>(state.completions.size())) {
            const std::string& completion = state.completions[state.suggestion_selected];
            if (!CommandPrefixOf(state.content).empty()) {
                // A command line IS its prefix: whole-line replacement.
                state.content = completion;
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
            }
        }
        state.list_open = false;
        state.suggestions.clear();
        state.completions.clear();
    };

    // accept_suggestion captured BY VALUE (copy), not by reference: it is a
    // local closure object in THIS function's stack frame, unlike `state`
    // (a reference parameter binding to an object that outlives this
    // factory). A `[&]` capture here would store a reference to a stack
    // variable that dies the moment MakePromptEditor returns -- exactly the
    // "closures outlive the factory" trap session_picker.cpp documents.
    // Copying is cheap: accept_suggestion's own captures (state, etc.) are
    // themselves references to externally-owned objects.
    auto with_events = CatchEvent(input, [&, accept_suggestion](const Event& event) {
        // Alt+Enter (ESC CR) inserts a newline -- the portable multi-line key.
        static const Event kAltEnterCr = Event::Character("\x1b\r");
        static const Event kAltEnterLf = Event::Character("\x1b\n");
        if (event == kAltEnterCr || event == kAltEnterLf) {
            state.content += "\n";
            return true;   // consumed
        }
        // Enter accepts the highlighted suggestion when the list is open,
        // the same as Tab, rather than submitting the raw (incomplete)
        // prefix as a turn/command -- Enter is the more natural "I've chosen
        // this" key, and submitting `/comp` (not a real command) when the
        // user was reaching for `/compact` from the open list was a silent
        // trap. Only when the list is CLOSED does Enter fall through to
        // submit, unchanged from before.
        if (state.list_open && !state.suggestions.empty() && event == Event::Return) {
            accept_suggestion();
            return true;
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
            return true;
        }

        // After every character edit: either the @-mention debounce re-arms
        // (the query itself fires from the render gate below -- no timers, no
        // threads; frame timing carries it) or, for a '/'+name prefix, the
        // LOCAL command catalog filters immediately.
        if (event.is_character()) {
            // Editing a recalled line exits browsing -- it is no longer the
            // same text as history[cursor], so further up/down should not
            // silently discard what was just typed.
            state.history_cursor.reset();
            const std::string command_prefix = CommandPrefixOf(state.content);
            if (!command_prefix.empty() && command_catalog != nullptr) {
                state.suggestions.clear();
                state.completions.clear();
                for (const auto& entry : command_catalog()) {
                    if (entry.name.rfind(command_prefix, 0) != 0) continue;
                    std::string display = "/" + entry.name + " -- " + entry.summary;
                    if (entry.submits_prompt) display += "  (submits a prompt as your turn)";
                    if (entry.read_only) display += "  (read-only)";
                    state.suggestions.push_back(std::move(display));
                    state.completions.push_back("/" + entry.name + " ");
                }
                state.list_open = !state.suggestions.empty();
                state.suggestion_selected = 0;
                state.query_pending = false;
                return false;   // the character itself still goes to the Input
            }

            state.pending_prefix = MentionPrefixAt(state.content);
            state.query_pending = !state.pending_prefix.empty();
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

        // The debounce gate: once the delay has passed and the prefix is
        // unchanged, ask the server. A failed/empty answer closes the list.
        if (state.query_pending && std::chrono::steady_clock::now() >= state.query_at) {
            state.suggestions = suggest(state.pending_prefix);
            state.completions = state.suggestions;
            state.query_pending = false;
            state.list_open = !state.suggestions.empty();
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
                if (selected) item |= bold;
                items.push_back(std::move(item));
            }
            lines.push_back(vbox(std::move(items)));
        }
        return vbox(std::move(lines));
    });
}

}  // namespace minimal::client
