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

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "minimal/client/key_event_parser.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/prompt_editor.hpp"

using minimal::client::ColorDepth;
using minimal::client::MakePromptEditor;
using minimal::client::PromptEditorState;
using minimal::client::PromptUndo;
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

TEST_CASE("PromptEditor: key_matches overrides the default Alt+Enter list",
          "[prompt_editor]") {
    // The "prompt.newline" rebind path (tui_app.cpp wires the keymap
    // here): a null callback (every test above) falls back to the hardcoded
    // default; a non-null one replaces it entirely, so a spec that ISN'T in
    // the default list can insert a newline (via InsertNewlineAtCursor,
    // splicing at the cursor), and Alt+Enter -- no longer recognized once
    // overridden -- falls through to the Input's plain-character handling
    // instead (the raw ESC/CR bytes get typed in verbatim, not a clean
    // newline split).
    const auto only_ctrl_n = [](std::string_view action, const ftxui::Event& event) {
        return action == "prompt.newline" &&
               event == ftxui::Event::Special(std::string(1, '\x0E'));  // ctrl+n
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

namespace {

ftxui::Event Key(const std::string& spec) {
    const auto events = minimal::client::ParseKeySpec(spec);
    REQUIRE_FALSE(events.empty());
    return events.front();
}

void TypeInto(ftxui::Component& editor, const std::string& text) {
    for (const char c : text) {
        REQUIRE(editor->OnEvent(ftxui::Event::Character(std::string(1, c))));
    }
}

}  // namespace

TEST_CASE("PromptEditor: undo reverts a word at a time, redo re-applies",
          "[prompt_editor][undo]") {
    PromptEditorState state;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});
    TypeInto(editor, "hello world");

    // The typed space starts a new step: one undo takes back " world".
    CHECK(editor->OnEvent(Key("alt+z")));
    CHECK(state.content == "hello");
    CHECK(state.cursor_position == 5);
    CHECK(editor->OnEvent(Key("alt+z")));
    CHECK(state.content.empty());
    CHECK(state.cursor_position == 0);

    CHECK(editor->OnEvent(Key("ctrl+alt+z")));
    CHECK(state.content == "hello");
    CHECK(editor->OnEvent(Key("ctrl+alt+z")));
    CHECK(state.content == "hello world");
    CHECK(state.cursor_position == 11);
}

TEST_CASE("PromptEditor: an edit after undo drops the redo stack", "[prompt_editor][undo]") {
    PromptEditorState state;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});
    TypeInto(editor, "ab");
    REQUIRE(editor->OnEvent(Key("alt+z")));
    REQUIRE(state.content.empty());
    TypeInto(editor, "c");
    CHECK(state.undo.redo_depth() == 0);
    // Redo has nothing to re-apply, but the key is still consumed (never
    // typed into the prompt as text).
    CHECK(editor->OnEvent(Key("ctrl+alt+z")));
    CHECK(state.content == "c");
}

TEST_CASE("PromptEditor: submit clears the undo history", "[prompt_editor][undo]") {
    PromptEditorState state;
    std::string submitted;
    auto editor = MakePromptEditor(TestTheme(), state,
                                   [&](const std::string& text) { submitted = text; }, {}, {});
    TypeInto(editor, "sent");
    REQUIRE(editor->OnEvent(ftxui::Event::Return));
    REQUIRE(submitted == "sent");
    CHECK(state.undo.undo_depth() == 0);
    CHECK(state.undo.redo_depth() == 0);
    CHECK(editor->OnEvent(Key("alt+z")));
    CHECK(state.content.empty());
}

TEST_CASE("PromptEditor: kill line and paste are one undo step each", "[prompt_editor][undo]") {
    PromptEditorState state;
    auto editor = MakePromptEditor(TestTheme(), state, {}, {}, {});
    TypeInto(editor, "abc");
    REQUIRE(editor->OnEvent(ftxui::Event::Character(std::string(1, '\x15'))));   // ctrl+u
    REQUIRE(state.content.empty());
    CHECK(editor->OnEvent(Key("alt+z")));
    CHECK(state.content == "abc");

    // A bracketed paste, spaces and all, reverts as a whole.
    REQUIRE(editor->OnEvent(ftxui::Event::Special("\x1b[200~")));
    TypeInto(editor, " x y");
    REQUIRE(editor->OnEvent(ftxui::Event::Special("\x1b[201~")));
    REQUIRE(state.content == "abc x y");
    CHECK(editor->OnEvent(Key("alt+z")));
    CHECK(state.content == "abc");
}

TEST_CASE("PromptUndo: a pause or a caret jump starts a new step", "[prompt_editor][undo]") {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    PromptUndo undo;
    undo.Record({"", 0}, PromptUndo::Kind::Insert, 1, false, t0);
    undo.Record({"a", 1}, PromptUndo::Kind::Insert, 2, false, t0 + std::chrono::milliseconds(100));
    CHECK(undo.undo_depth() == 1);
    // Past the coalesce gap.
    undo.Record({"ab", 2}, PromptUndo::Kind::Insert, 3, false, t0 + std::chrono::seconds(3));
    CHECK(undo.undo_depth() == 2);
    // Typing somewhere other than where the run left the caret.
    undo.Record({"abc", 0}, PromptUndo::Kind::Insert, 1, false, t0 + std::chrono::seconds(3));
    CHECK(undo.undo_depth() == 3);
}

TEST_CASE("PromptUndo: the history is capped", "[prompt_editor][undo]") {
    PromptUndo undo;
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < minimal::client::kMaxUndoSteps + 20; ++i) {
        undo.Record({std::to_string(i), 0}, PromptUndo::Kind::Other, 0, false, now);
    }
    CHECK(undo.undo_depth() == minimal::client::kMaxUndoSteps);
    // The oldest steps went first.
    std::optional<PromptUndo::Snapshot> last;
    while (auto step = undo.Undo({"current", 0})) last = step;
    REQUIRE(last.has_value());
    CHECK(last->content == "20");
}

TEST_CASE("PromptEditor: a sensitive command's arguments stay out of the history",
          "[prompt_editor][history]") {
    using minimal::client::CommandCatalogEntry;
    auto catalog = std::make_shared<const std::vector<CommandCatalogEntry>>(
        std::vector<CommandCatalogEntry>{
            {"secret", "store a secret", true, false, false, /*sensitive_args=*/true},
            {"model", "pick a model", false, false, true, false}});
    minimal::client::PromptHistory hist;
    PromptEditorState state;
    auto editor = MakePromptEditor(
        TestTheme(), state, {}, [](const std::string&) {}, {}, [&] { return catalog; },
        nullptr, nullptr, nullptr, &hist);
    const auto submit = [&](const std::string& line) {
        state.content = line;
        state.cursor_position = static_cast<int>(line.size());
        REQUIRE(editor->OnEvent(ftxui::Event::Return));
    };

    submit("/secret set openai sk-live-123");
    submit("/model local/llama");
    CHECK(hist.entries == std::vector<std::string>{"/secret", "/model local/llama"});

    // Before the catalog arrives, no /-command's arguments are kept.
    catalog = std::make_shared<const std::vector<CommandCatalogEntry>>();
    submit("/secret set other sk-live-456");
    CHECK(hist.entries.back() == "/secret");
}
