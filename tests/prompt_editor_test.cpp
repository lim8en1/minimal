// Behavior tier for the prompt editor (src/client/view/prompt_editor.cpp):
// newline-key handling and the soft-wrapped render, exercised through the
// real component (MakePromptEditor) with a rendered screen -- the same tier
// checklist_view_test uses for its panel view.
//
// The newline cases are the point: Alt+Enter's several wire encodings must
// insert a newline while PLAIN Enter must submit, byte for byte -- a parser
// that gained CSI-u but lost plain \r would be a regression wearing a
// feature's clothes. The render cases lock in the soft wrap: FTXUI's Input
// renders one terminal row per logical line, so a long prompt used to scroll
// horizontally and could not be reviewed before sending.
//
// The wrap width comes from FTXUI's Terminal::Size() at render time; with
// stdout a pipe (CI, ctest) that is the 80x24 fallback, so multi-row
// assertions hold for any terminal narrower than the fixture's 1000 columns.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/prompt_editor.hpp"

using minimal::client::ColorDepth;
using minimal::client::MakePromptEditor;
using minimal::client::PromptEditorState;
using minimal::client::Theme;

namespace {

const Theme& TestTheme() {
    static const Theme theme =
        Theme::Parse(R"({"name": "test", "colors": {"error": "#f00"}})",
                     ColorDepth::Truecolor);
    return theme;
}

std::string ToText(ftxui::Component& component, int width, int height) {
    auto element = component->Render();
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen.ToString();
}

}  // namespace

TEST_CASE("PromptEditor: Alt+Enter (ESC CR) inserts a newline", "[prompt_editor]") {
    PromptEditorState state;
    state.content = "ab";
    state.cursor_position = 1;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

    CHECK(editor->OnEvent(ftxui::Event::Character("\x1b\r")));
    CHECK(state.content == "a\nb");
    CHECK(state.cursor_position == 2);
    CHECK(state.content == "a\nb");   // not submitted: the draft survives
}

TEST_CASE("PromptEditor: Alt+Enter (ESC LF) inserts a newline", "[prompt_editor]") {
    PromptEditorState state;
    state.content = "ab";
    state.cursor_position = 2;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

    CHECK(editor->OnEvent(ftxui::Event::Character("\x1b\n")));
    CHECK(state.content == "ab\n");
    CHECK(state.cursor_position == 3);
}

TEST_CASE("PromptEditor: CSI-u alt/shift enter encodings insert a newline",
          "[prompt_editor]") {
    // The kitty keyboard protocol (\x1b[13;{mod}u) and xterm modifyOtherKeys
    // (\x1b[27;{mod};13~) forms: the encodings a terminal reports when it
    // DOES distinguish Option/Shift+Enter.
    for (const std::string bytes :
         {"\x1b[13;3u", "\x1b[13;2u", "\x1b[27;3;13~", "\x1b[27;2;13~"}) {
        SECTION(std::string("encoding ") + bytes) {
            PromptEditorState state;
            state.content = "ab";
            state.cursor_position = 1;
            auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

            CHECK(editor->OnEvent(ftxui::Event::Special(bytes)));
            CHECK(state.content == "a\nb");
        }
    }
}

TEST_CASE("PromptEditor: is_newline_event overrides the default Alt+Enter list",
          "[prompt_editor]") {
    // The "prompt.newline" rebind path (app.cpp wires a keymap-backed lambda
    // here): a null callback (every test above) falls back to the hardcoded
    // default; a non-null one replaces it entirely, so a spec that ISN'T in
    // the default list can insert a newline (via InsertNewlineAtCursor,
    // splicing at the cursor), and Alt+Enter -- no longer recognized once
    // overridden -- falls through to the Input's plain-character handling
    // instead (the raw ESC/CR bytes get typed in verbatim, not a clean
    // newline split).
    const auto only_ctrl_n = [](const ftxui::Event& event) {
        return event == ftxui::Event::Special(std::string(1, '\x0E'));  // ctrl+n
    };

    PromptEditorState alt_enter_state;
    alt_enter_state.content = "ab";
    alt_enter_state.cursor_position = 1;
    auto alt_enter_editor = MakePromptEditor(TestTheme(), alt_enter_state, {}, {}, {}, nullptr,
                                             nullptr, nullptr, nullptr, nullptr, only_ctrl_n);
    CHECK(alt_enter_editor->OnEvent(ftxui::Event::Character("\x1b\r")));  // consumed as plain text
    CHECK(alt_enter_state.content != "a\nb");   // NOT the newline-insert splice anymore

    PromptEditorState ctrl_n_state;
    ctrl_n_state.content = "ab";
    ctrl_n_state.cursor_position = 1;
    auto ctrl_n_editor = MakePromptEditor(TestTheme(), ctrl_n_state, {}, {}, {}, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, only_ctrl_n);
    CHECK(ctrl_n_editor->OnEvent(ftxui::Event::Special(std::string(1, '\x0E'))));
    CHECK(ctrl_n_state.content == "a\nb");
}

TEST_CASE("PromptEditor: plain Enter submits and clears", "[prompt_editor]") {
    PromptEditorState state;
    state.content = "hello";
    state.cursor_position = 5;
    std::string submitted;
    auto editor = MakePromptEditor(TestTheme(), state,
                                   [&](const std::string& text) { submitted = text; }, {}, {});

    CHECK(editor->OnEvent(ftxui::Event::Return));
    CHECK(submitted == "hello");
    CHECK(state.content.empty());
}

TEST_CASE("PromptEditor: plain Enter on an empty buffer submits nothing",
          "[prompt_editor]") {
    PromptEditorState state;
    bool called = false;
    auto editor = MakePromptEditor(TestTheme(), state,
                                   [&](const std::string&) { called = true; },
                                   [&](const std::string&) { called = true; }, {});

    CHECK(editor->OnEvent(ftxui::Event::Return));
    CHECK_FALSE(called);
    CHECK(state.content.empty());
}

TEST_CASE("PromptEditor: the soft-wrapped body renders one row per visual line",
          "[prompt_editor]") {
    // 1000 columns is wider than any real terminal the fallback could miss:
    // whatever width Terminal::Size() reports (< 1000), the content wraps to
    // more than one visual row.
    PromptEditorState state;
    state.content = std::string(1000, 'x');
    state.cursor_position = 0;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

    const std::string rendered = ToText(editor, 80, 20);
    int x_rows = 0;
    std::size_t pos = 0;
    while (pos < rendered.size()) {
        const std::size_t newline = rendered.find('\n', pos);
        const std::string line =
            rendered.substr(pos, newline == std::string::npos ? std::string::npos
                                                              : newline - pos);
        if (line.find('x') != std::string::npos) ++x_rows;
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    CHECK(x_rows >= 2);
    // Half-screen cap (24-line fallback -> 12) bounds the editor even for a
    // draft this long; the visible rows cannot exceed it.
    CHECK(x_rows <= 12);
}

TEST_CASE("PromptEditor: an empty prompt renders the placeholder", "[prompt_editor]") {
    PromptEditorState state;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

    const std::string rendered = ToText(editor, 80, 6);
    CHECK(rendered.find("type here; @ for files") != std::string::npos);
}

TEST_CASE("PromptEditor: a multi-line draft renders each logical line", "[prompt_editor]") {
    PromptEditorState state;
    state.content = "first line\nsecond line";
    state.cursor_position = static_cast<int>(state.content.size());
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});

    const std::string rendered = ToText(editor, 80, 6);
    CHECK(rendered.find("first line") != std::string::npos);
    CHECK(rendered.find("second line") != std::string::npos);
}
