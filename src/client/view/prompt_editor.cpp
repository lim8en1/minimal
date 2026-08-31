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
    std::function<std::vector<std::string>(const std::string& prefix)> suggest,
    std::function<std::vector<CommandCatalogEntry>()> command_catalog) {
    using namespace ftxui;

    // Multiline ON: Return-as-newline is the Input's default, so the submit
    // decision is ours -- CatchEvent sees every event first.
    auto input = Input(&state.content,
                       "type here; @ for files, / for commands, alt+enter for newline");

    auto with_events = CatchEvent(input, [&](const Event& event) {
        // Alt+Enter (ESC CR) inserts a newline -- the portable multi-line key.
        static const Event kAltEnterCr = Event::Character("\x1b\r");
        static const Event kAltEnterLf = Event::Character("\x1b\n");
        if (event == kAltEnterCr || event == kAltEnterLf) {
            state.content += "\n";
            return true;   // consumed
        }
        if (event == Event::Return) {
            std::string line = state.content;
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
                line.pop_back();
            }
            if (line.empty()) return true;
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
            // Complete the highlighted suggestion in place. Completions may
            // differ from the displayed text (a command completes to its name
            // plus a space, not to the annotation line).
            if (state.suggestion_selected < static_cast<int>(state.completions.size())) {
                state.content = state.completions[state.suggestion_selected];
            }
            state.list_open = false;
            state.suggestions.clear();
            state.completions.clear();
            return true;   // consumed
        }

        // After every character edit: either the @-mention debounce re-arms
        // (the query itself fires from the render gate below -- no timers, no
        // threads; frame timing carries it) or, for a '/'+name prefix, the
        // LOCAL command catalog filters immediately.
        if (event.is_character()) {
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
            if (!state.list_open) {
                state.suggestions.clear();
                state.completions.clear();
            }
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
