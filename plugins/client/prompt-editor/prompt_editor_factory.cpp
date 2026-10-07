#include "prompt_editor_factory.hpp"

#include <atomic>

#include "minimal/client/key_event_parser.hpp"
#include "prompt_editor.hpp"

namespace minimal::client::prompt_editor {

namespace {

class Editor final : public plugins::PromptEditor {
public:
    Editor(plugins::PromptEditorHost& host, const Theme& theme)
        : component_(MakePromptEditor(theme, state_, host, pasting_)) {}

    ftxui::Component component() override { return component_; }
    std::string Text() const override { return state_.content; }
    void SetText(const std::string& text) override {
        state_.content = text;
        state_.cursor_position = static_cast<int>(state_.content.size());
        state_.history_cursor.reset();
    }

    bool Claims(const ftxui::Event& event) const override {
        using ftxui::Event;
        // Mid-paste, a macOS Option-glyph byte is byte-identical to the
        // same alt+key actually pressed: pasted text, not a bound action.
        // (The real ESC-prefixed alt encoding is a different Event that
        // never arrives mid-paste, so a genuine alt+key still dispatches.)
        if (*pasting_ && IsOptionGlyphEvent(event)) return true;
        // An open @/-suggestion list owns its navigation, accept and dismiss
        // keys ahead of every binding (busy-turn Escape, a focused panel's
        // Tab/arrows/Return).
        return state_.list_open &&
               (event == Event::Tab || event == Event::ArrowUp || event == Event::ArrowDown ||
                event == Event::Escape || event == Event::Return);
    }

    bool WantsFrames() const override {
        return state_.query_pending_hint.load(std::memory_order_acquire);
    }

    // The modal will swallow the paste's end marker.
    void OnFocusLost() override { *pasting_ = false; }

private:
    PromptEditorState state_;
    std::shared_ptr<bool> pasting_ = std::make_shared<bool>(false);
    ftxui::Component component_;   // last: built over state_ and pasting_
};

}  // namespace

std::vector<plugins::DeclaredAction> Factory::Actions() const {
    return {
        // Plain Enter submits; this inserts a line. See prompt_editor.hpp's
        // NEWLINE comment for why alt+enter, and the macOS caveat.
        {"prompt.newline", "alt+enter", "insert a newline"},
        // Not ctrl+z: the terminal's suspend key.
        {"prompt.undo", "alt+z", "undo the last prompt edit"},
        {"prompt.redo", "ctrl+alt+z", "redo the undone prompt edit"},
        // Not escape (turn.cancel). readline's ctrl+u widened to the whole
        // buffer, in the ctrl+alt+letter family.
        {"prompt.clear", "ctrl+alt+u", "clear the prompt"},
    };
}

std::unique_ptr<plugins::PromptEditor> Factory::Make(plugins::PromptEditorHost& host,
                                                     const Theme& theme) const {
    return std::make_unique<Editor>(host, theme);
}

}  // namespace minimal::client::prompt_editor
