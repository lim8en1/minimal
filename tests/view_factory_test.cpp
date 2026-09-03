// Headless FTXUI view-factory tests: the tier where track E's crashes lived.
// Track R1a (see .opencode/plans/track-r-remediation.md): every view factory is
// constructed from plain data, event-driven, and rendered to text with no
// terminal -- testing WIRING AND LIFETIME, not visuals (ARCHITECTURE.md's
// "FTXUI render output isn't unit-tested" excludes aesthetics, not this).
//
// Each invariant below was proven load-bearing by reverting its fix and
// watching the test fail (§6):
//   * picker highlighted-row resume -- revert to the by-value `selected`
//     capture and Enter resumes row 0.
//   * permission modal outliving the request snapshot -- revert the request
//     copy and the modal reads freed memory (ASan) / renders garbage.
//   * chat view (session_id, version) cache key -- revert the session-id
//     check and a colliding version serves the previous tab's rows.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "ftxui_harness.hpp"

#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/commands_help.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/session_picker.hpp"

using namespace minimal::client;
using namespace minimal_test;
namespace ftx = ftxui;
namespace v1 = minimal::v1;

namespace {

Theme TestTheme() {
    return Theme::Parse(R"({"name":"test","colors":{}})", ColorDepth::Truecolor);
}

v1::SessionSummary MakeSummary(const std::string& id, const std::string& name, int msgs) {
    v1::SessionSummary s;
    s.set_session_id(id);
    s.set_name(name);
    s.set_message_count(msgs);
    s.set_updated_at_unix_ms(1700000000000LL);
    return s;
}

v1::GrantOffer MakeGrant(const std::string& key, const std::string& label) {
    v1::GrantOffer offer;
    offer.set_key(key);
    offer.set_label(label);
    return offer;
}

PendingPermission MakePermissionRequest() {
    PendingPermission request;
    request.request_id = "perm-1";
    request.tool_name = "bash";
    request.summary = "run make";
    request.complete = false;   // the lower-bound paragraph must render
    request.grants = {MakeGrant("bash:/usr/bin/make", "/usr/bin/make"),
                      MakeGrant("bash:/usr/bin/tail", "/usr/bin/tail")};
    return request;
}

v1::StoredMessage StoredUserText(int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_USER);
    auto* block = stored.mutable_message()->add_content();
    block->mutable_text()->set_text(text);
    return stored;
}

// A completed call to a tool with no NAME-specific ToolResultRenderer
// registered -- true of every BUILT-IN tool (bash, edit, read, ...), since
// that registry is plugin-only. Two messages: the assistant's tool_use (seq)
// and the user's tool_result (seq+1) that resolves it into ONE ToolActivity
// row (ChatRowsFromMessages drops the bare tool_use row once its result is
// in the same page).
std::vector<v1::StoredMessage> StoredToolCallAndResult(int64_t seq, const std::string& tool_name,
                                                        const std::string& result_text) {
    v1::StoredMessage call;
    call.set_seq(seq);
    call.mutable_message()->set_role(v1::ROLE_ASSISTANT);
    auto* use = call.mutable_message()->add_content()->mutable_tool_use();
    use->set_id("call-1");
    use->set_name(tool_name);

    v1::StoredMessage result;
    result.set_seq(seq + 1);
    result.mutable_message()->set_role(v1::ROLE_USER);
    auto* tool_result = result.mutable_message()->add_content()->mutable_tool_result();
    tool_result->set_tool_use_id("call-1");
    tool_result->add_content()->mutable_text()->set_text(result_text);

    // Descending by seq, newest first -- matches a real scrollback page.
    return {std::move(result), std::move(call)};
}

SessionState::Snapshot SnapshotWithText(const std::string& session_id, uint64_t version,
                                        const std::string& text) {
    SessionState::Snapshot snap;
    snap.session_id = session_id;
    snap.transcript_version = version;
    snap.scrollback_page =
        std::make_shared<const std::vector<v1::StoredMessage>>(
            std::vector<v1::StoredMessage>{StoredUserText(1, text)});
    return snap;
}

// `count` distinct rows. scrollback_page is DESCENDING by seq (newest at
// index 0 -- see ApplyScrollbackPage/state.cpp, and ChatRowsFromMessages's
// own `descending` parameter name), so the newest row ("row-<count-1>") goes
// in FIRST here to match what a real snapshot looks like; ChatRowsFromMessages
// reverses it back to ascending (oldest first, top-to-bottom chat order) for
// rendering. Enough rows that a small viewport cannot show all at once, so a
// pin-to-bottom vs. scrolled comparison is meaningful.
SessionState::Snapshot SnapshotWithRows(const std::string& session_id, uint64_t version,
                                        int count) {
    SessionState::Snapshot snap;
    snap.session_id = session_id;
    snap.transcript_version = version;
    std::vector<v1::StoredMessage> page;
    for (int i = count - 1; i >= 0; --i) {
        page.push_back(StoredUserText(i + 1, "row-" + std::to_string(i)));
    }
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::move(page));
    return snap;
}

}  // namespace

// ---------------------------------------------------------------------------
// Session picker (R1b-2)
// ---------------------------------------------------------------------------

namespace {

SessionPickerCallbacks MakePickerCallbacks(std::vector<std::string>& resumed,
                                           std::vector<std::string>& deleted, int& closes,
                                           int& new_sessions, int& all_dirs_toggles,
                                           int& load_mores) {
    SessionPickerCallbacks callbacks;
    callbacks.on_resume = [&](const std::string& id) { resumed.push_back(id); };
    callbacks.on_delete = [&](const std::string& id) { deleted.push_back(id); };
    callbacks.on_new = [&] { ++new_sessions; };
    callbacks.on_close = [&] { ++closes; };
    callbacks.on_toggle_all_dirs = [&] { ++all_dirs_toggles; };
    callbacks.on_load_more = [&] { ++load_mores; };
    return callbacks;
}

}  // namespace

TEST_CASE("session picker: Enter with no navigation opens a NEW session, not the first stored "
         "one",
         "[session_picker][regression]") {
    // Generalizes the picker's original resume-row regression: row 0 is now
    // the synthetic "+ new session" row (T8), so the failure mode this
    // guards is Enter silently resuming whatever the FIRST stored session
    // happens to be instead of doing what the highlighted row says.
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {MakeSummary("s0", "alpha", 1)};
    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        sessions, /*include_all_dirs=*/false, /*has_more=*/false, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));

    const auto frame = RenderFrame(picker);
    CHECK(frame.find("+ new session") != std::string::npos);
    CHECK(frame.find("alpha") != std::string::npos);

    REQUIRE(SendEvent(picker, ftx::Event::Return));
    CHECK(new_sessions == 1);
    CHECK(resumed.empty());
}

TEST_CASE("session picker: Enter resumes the HIGHLIGHTED row, not row 0") {
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {
        MakeSummary("s0", "alpha", 1), MakeSummary("s1", "beta", 2),
        MakeSummary("s2", "gamma", 3)};

    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        sessions, /*include_all_dirs=*/false, /*has_more=*/false, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));

    const auto frame = RenderFrame(picker);
    CHECK(frame.find("alpha") != std::string::npos);
    CHECK(frame.find("gamma") != std::string::npos);

    // Row 0 is "+ new session" (T8); down twice reaches "s1" (row index 2).
    // Enter must resume THAT row. The pre-fix code froze the CatchEvent's
    // copy of `selected` at 0.
    REQUIRE(SendEvent(picker, ftx::Event::ArrowDown));
    REQUIRE(SendEvent(picker, ftx::Event::ArrowDown));
    REQUIRE(SendEvent(picker, ftx::Event::Return));
    REQUIRE(resumed.size() == 1);
    CHECK(resumed[0] == "s1");

    // The picker is still alive and stateful after resuming: ctrl+d nominates
    // the highlighted row too (the confirmation modal is RunTui's). Plain 'd'
    // no longer deletes (T8: it types into the filter instead).
    REQUIRE(SendEvent(picker, ftx::Event::CtrlD));
    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == "s1");

    REQUIRE(SendEvent(picker, ftx::Event::Escape));
    CHECK(closes == 1);
}

TEST_CASE("session picker: a bare 'd' types into the filter instead of deleting",
         "[session_picker][regression]") {
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {MakeSummary("s0", "delta", 1)};
    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        sessions, /*include_all_dirs=*/false, /*has_more=*/false, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));

    REQUIRE(SendEvent(picker, ftx::Event::Character("d")));
    CHECK(deleted.empty());
    const auto frame = RenderFrame(picker);
    CHECK(frame.find("filter: d") != std::string::npos);
    // "delta" still matches the filter "d"; the row survives.
    CHECK(frame.find("delta") != std::string::npos);

    // Backspace clears it back out.
    REQUIRE(SendEvent(picker, ftx::Event::Backspace));
    CHECK(RenderFrame(picker).find("filter: (none)") != std::string::npos);
}

TEST_CASE("session picker: an empty list still offers the new-session row, and consumes "
         "Escape without closing twice") {
    const auto theme = TestTheme();
    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        {}, /*include_all_dirs=*/false, /*has_more=*/false, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));
    CHECK(RenderFrame(picker).find("no other stored sessions") != std::string::npos);
    REQUIRE(SendEvent(picker, ftx::Event::Return));
    CHECK(new_sessions == 1);   // the only row IS the new-session row
    CHECK(resumed.empty());
    REQUIRE(SendEvent(picker, ftx::Event::Escape));
    CHECK(closes == 1);
}

TEST_CASE("session picker: tab toggles sort; ctrl+a asks the host to toggle all-dirs") {
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {MakeSummary("s0", "alpha", 1)};
    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        sessions, /*include_all_dirs=*/false, /*has_more=*/false, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));

    CHECK(RenderFrame(picker).find("sort: recent") != std::string::npos);
    REQUIRE(SendEvent(picker, ftx::Event::Tab));
    CHECK(RenderFrame(picker).find("sort: name") != std::string::npos);

    REQUIRE(SendEvent(picker, ftx::Event::CtrlA));
    CHECK(all_dirs_toggles == 1);
}

TEST_CASE("session picker: arrowing down past the last loaded row asks for more, only when "
         "unfiltered") {
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {MakeSummary("s0", "alpha", 1)};
    std::vector<std::string> resumed, deleted;
    int closes = 0, new_sessions = 0, all_dirs_toggles = 0, load_mores = 0;
    auto picker = MakeSessionPicker(
        sessions, /*include_all_dirs=*/false, /*has_more=*/true, theme,
        MakePickerCallbacks(resumed, deleted, closes, new_sessions, all_dirs_toggles,
                            load_mores));

    // Rows: [new-session, s0]. One ArrowDown reaches the last row; a SECOND
    // ArrowDown (already there) is what should trigger "load more".
    REQUIRE(SendEvent(picker, ftx::Event::ArrowDown));
    CHECK(load_mores == 0);
    REQUIRE(SendEvent(picker, ftx::Event::ArrowDown));
    CHECK(load_mores == 1);

    // Typing a filter suppresses it: a local filter hiding rows is not the
    // server having more pages. (Not REQUIRE'd on this ArrowDown's own
    // return value: with the filter narrowing the list to one row, Menu has
    // nowhere to move and may report the event as unconsumed -- the
    // assertion that matters is that load_mores did not fire again.)
    REQUIRE(SendEvent(picker, ftx::Event::Character("z")));
    SendEvent(picker, ftx::Event::ArrowDown);
    CHECK(load_mores == 1);
}

// ---------------------------------------------------------------------------
// Permission modal (R1b-3)
// ---------------------------------------------------------------------------

TEST_CASE("permission modal: answers after the caller's request snapshot dies") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    auto request = std::make_unique<PendingPermission>(MakePermissionRequest());

    // The callback owns its copy (RunTui's does too): GrantedKeys reads the
    // request AFTER the caller's snapshot is gone.
    const PendingPermission answer_source = *request;
    std::optional<bool> answered;
    std::vector<std::string> keys;
    auto modal = MakePermissionModal(*request, toggles, theme, [&](bool allow) {
        answered = allow;
        keys = toggles.GrantedKeys(answer_source);
    });

    request.reset();   // the per-frame snapshot dies; the modal must not care

    const auto frame = RenderFrame(modal);
    CHECK(frame.find("bash") != std::string::npos);
    CHECK(frame.find("may reach more") != std::string::npos);   // lower-bound obligation
    CHECK(frame.find("session grants:") != std::string::npos);  // per-binary offers present

    // Toggle the first grant (space on the focused checkbox), then allow: the
    // granted key is exactly what was offered, never minted.
    REQUIRE(SendEvent(modal, ftx::Event::Character(" ")));
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answered.has_value());
    CHECK(*answered);
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("permission modal: Escape denies without recording grants") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<bool> answered;
    auto modal = MakePermissionModal(request, toggles, theme,
                                     [&](bool allow) { answered = allow; });
    REQUIRE(SendEvent(modal, ftx::Event::Escape));
    REQUIRE(answered.has_value());
    CHECK_FALSE(*answered);
}

TEST_CASE("permission modal: with grants offered, the footer advertises tab/space") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();  // has two grants
    auto modal = MakePermissionModal(request, toggles, theme, [](bool) {});
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("tab/arrow: move") != std::string::npos);
    CHECK(frame.find("toggle grants") != std::string::npos);
}

TEST_CASE("permission modal: with no grants offered, the footer omits the dead "
          "tab/space affordances") {
    // Regression: the footer used to always advertise "tab/arrow: move" and
    // "space: toggle grants", even on a request with nothing to move between
    // or toggle -- dead keys advertised on the security-decision surface.
    const auto theme = TestTheme();
    GrantToggles toggles;
    auto request = MakePermissionRequest();
    request.grants.clear();
    auto modal = MakePermissionModal(request, toggles, theme, [](bool) {});
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("enter: allow") != std::string::npos);
    CHECK(frame.find("esc: deny") != std::string::npos);
    CHECK(frame.find("tab/arrow: move") == std::string::npos);
    CHECK(frame.find("toggle grants") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Confirmation modal
// ---------------------------------------------------------------------------

TEST_CASE("confirmation modal: renders the ask and answers the focused option") {
    const auto theme = TestTheme();
    PendingConfirmation request;
    request.kind = "app_quit";
    request.title = "destroy running work?";
    request.detail = "a session still has work in flight";
    request.options = {"close anyway", "cancel"};

    std::vector<std::string> answers;
    auto modal = MakeConfirmationModal(
        request, theme, [&](const std::string& chosen) { answers.push_back(chosen); });

    const auto frame = RenderFrame(modal);
    CHECK(frame.find("destroy running work?") != std::string::npos);
    CHECK(frame.find("close anyway") != std::string::npos);

    REQUIRE(SendEvent(modal, ftx::Event::Return));   // focused button: the first option
    REQUIRE(answers.size() == 1);
    CHECK(answers[0] == "close anyway");
}

// ---------------------------------------------------------------------------
// Prompt editor
// ---------------------------------------------------------------------------

TEST_CASE("prompt editor: Enter submits text, / lines go to the command path") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; });

    REQUIRE(SendEvent(editor, ftx::Event::Character("h")));
    REQUIRE(SendEvent(editor, ftx::Event::Character("i")));
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(texts.size() == 1);
    CHECK(texts[0] == "hi");
    CHECK(commands.empty());
    CHECK(state.content.empty());   // the line is consumed, not left to re-submit

    state.content = "/help";
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(commands.size() == 1);
    CHECK(commands[0] == "/help");
    CHECK(texts.size() == 1);   // a command never double-fires as text
}

// Regression: MakePromptEditor's event handler used to capture `[&]`, which
// binds references to the FACTORY'S OWN PARAMETERS (on_text, on_command,
// command_catalog -- by-value std::functions living in MakePromptEditor's
// stack frame). Every call in this file already exercises the callback
// lifetime implicitly (the editor Component is always used well after
// MakePromptEditor returns, the same as the TUI's real usage), so this is the
// harness that would have caught the dangling-reference bug had it been run
// under ASan's detect_stack_use_after_return -- this test names that
// property explicitly rather than leaving it merely incidental to every test
// above.
TEST_CASE("prompt editor: callbacks fire correctly well after MakePromptEditor returns") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    // MakePromptEditor itself is entered and exited here; `editor` is the
    // only thing that survives into the test body.
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); }, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    // Several turns through the loop, well past the factory call's own stack
    // frame -- each one exercises on_text through the captured closure.
    for (int i = 0; i < 3; ++i) {
        state.content = "turn " + std::to_string(i);
        REQUIRE(SendEvent(editor, ftx::Event::Return));
    }
    REQUIRE(texts.size() == 3);
    CHECK(texts[0] == "turn 0");
    CHECK(texts[2] == "turn 2");
}

TEST_CASE("prompt editor: Alt+Enter inserts a newline AT THE CURSOR, not appended to the end") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    state.content = "before after";
    state.cursor_position = 6;   // between "before" and " after"
    REQUIRE(SendEvent(editor, ftx::Event::Character("\x1b\r")));   // Alt+Enter (ESC CR)

    CHECK(state.content == "before\n after");   // split where the cursor was, not appended
    CHECK(state.cursor_position == 7);           // advanced past the inserted newline
}

TEST_CASE("prompt editor: history recall places the cursor at the end of the recalled line") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    // Submit a long line, then a short one, so recall moves the cursor
    // BACKWARD relative to wherever it was left after typing the short one --
    // FTXUI's Input only clamps cursor_position into content's new bounds
    // after an external splice, it never moves it, so a stale mid-word
    // position from editing "hi" would otherwise land mid-word in the
    // recalled "a much longer line" instead of at its end.
    for (const auto* line : {"a much longer line", "hi"}) {
        state.content = line;
        REQUIRE(SendEvent(editor, ftx::Event::Return));
    }
    state.cursor_position = 1;   // simulate wherever "hi"'s own edits left it

    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));    // recall "hi"
    CHECK(state.content == "hi");
    CHECK(state.cursor_position == 2);

    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));    // recall "a much longer line"
    CHECK(state.content == "a much longer line");
    CHECK(state.cursor_position == static_cast<int>(state.content.size()));

    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));  // back to "hi"
    CHECK(state.content == "hi");
    CHECK(state.cursor_position == 2);
}

// ---------------------------------------------------------------------------
// Chat view cache key (R1b-6's unit-testable half)
// ---------------------------------------------------------------------------

TEST_CASE("chat view: a session switch at an EQUAL transcript version re-renders") {
    const auto theme = TestTheme();
    minimal::client::MarkdownRenderer renderer(&theme, nullptr);
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
    auto& engine = *engine_registry.engine;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    ChatView view(theme, messages, tool_renderers);
    auto stream = engine.MakeStream();

    // Tab A, version 2. Then tab B, ALSO at version 2: the per-session
    // counter collides across tabs, so version alone cannot key the cache.
    const auto frame_a = RenderElement(view.Render(SnapshotWithText("session-a", 2, "alpha"),
                                                   *stream));
    CHECK(frame_a.find("alpha") != std::string::npos);

    const auto frame_b = RenderElement(view.Render(SnapshotWithText("session-b", 2, "beta"),
                                                   *stream));
    CHECK(frame_b.find("beta") != std::string::npos);   // fails pre-fix: stale "alpha"
    CHECK(frame_b.find("alpha") == std::string::npos);
}

TEST_CASE("chat view: a completed tool call with no name-specific renderer still renders "
         "(falls back to the message-kind default)") {
    const auto theme = TestTheme();
    minimal::client::MarkdownRenderer renderer(&theme, nullptr);
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
    auto& engine = *engine_registry.engine;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;   // deliberately empty: no plugin
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    ChatView view(theme, messages, tool_renderers);
    auto stream = engine.MakeStream();

    SessionState::Snapshot snap;
    snap.session_id = "session-tool";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "total 0\ndrwxr-xr-x  2 x  x  64 x x"));

    const auto frame = RenderElement(view.Render(snap, *stream));
    CHECK(frame.find("bash") != std::string::npos);

    // ToolActivity rows collapse by default (one-liner; expandable body) --
    // focus and expand this one to verify the fallback renderer's body
    // content, not just its header.
    view.MoveRowFocus(0);
    view.ToggleCollapseFocused();
    const auto expanded = RenderElement(view.Render(snap, *stream));
    CHECK(expanded.find("total 0") != std::string::npos);
}

TEST_CASE("chat view: ScrollBy moves the viewport off the pinned-to-newest default") {
    // Pre-fix, Render() always focus()ed the LAST row unconditionally, so no
    // key or mouse event could ever change what's visible -- this is the
    // regression test for that (arrow/mouse scrolling did nothing).
    const auto theme = TestTheme();
    minimal::client::MarkdownRenderer renderer(&theme, nullptr);
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
    auto& engine = *engine_registry.engine;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    ChatView view(theme, messages, tool_renderers);
    auto stream = engine.MakeStream();
    const auto snapshot = SnapshotWithRows("session-scroll", 1, 40);

    // A viewport far shorter than 40 rows: yframe can only show a slice, so
    // which slice is visible reflects where Render() put the focus marker
    // (ftxui's Frame::SetBox centers the focused row in the viewport, so
    // this doesn't assert an exact edge -- just that the visible slice
    // actually changes). Default (no ScrollBy call): pinned to the newest row.
    const auto pinned =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);
    CHECK(pinned.find("row-39") != std::string::npos);

    // Scroll toward older messages (positive == toward older), far enough
    // (30 of 40 rows) that an 8-line viewport cannot possibly show the same
    // slice: proves ScrollBy actually moves the anchor rather than a no-op.
    view.ScrollBy(30);
    const auto scrolled =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);
    CHECK(scrolled != pinned);
    CHECK(scrolled.find("row-39") == std::string::npos);

    // Scrolling back down by the same amount re-pins to the newest row,
    // reproducing the original (pre-scroll) view exactly.
    view.ScrollBy(-30);
    const auto repinned =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);
    CHECK(repinned == pinned);
}

TEST_CASE("chat view: ScrollToBottom resets the anchor directly, not just relatively") {
    // The scroll-position-drift UX fix: previously the only way back to the
    // newest row after scrolling up was walking down one row at a time (or
    // knowing exactly how far up you'd gone). ScrollToBottom() is bound to
    // chat.scroll_bottom (End) and also called on every chat turn submit.
    const auto theme = TestTheme();
    minimal::client::MarkdownRenderer renderer(&theme, nullptr);
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
    auto& engine = *engine_registry.engine;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    ChatView view(theme, messages, tool_renderers);
    auto stream = engine.MakeStream();
    const auto snapshot = SnapshotWithRows("session-scroll-bottom", 1, 40);

    const auto pinned =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);

    view.ScrollBy(30);   // scroll far up, same as the ScrollBy test above
    const auto scrolled =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);
    CHECK(scrolled != pinned);

    view.ScrollToBottom();   // one call, not "however far up ScrollBy went"
    const auto back =
        RenderElement(view.Render(snapshot, *stream) | ftx::yframe, /*width=*/40, /*height=*/8);
    CHECK(back == pinned);
    CHECK(back.find("row-39") != std::string::npos);
}

// ---------------------------------------------------------------------------
// @-mention completion (R5)
// ---------------------------------------------------------------------------

TEST_CASE("prompt editor: Tab splices the mention token, preserving the prose") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{"src/main.cpp"}; });

    // Type prose plus a partial mention.
    for (const char c : std::string_view("please review @src/ma")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    // The debounce gate fires from Render: wait out the 120 ms window.
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    RenderFrame(editor);
    REQUIRE_FALSE(state.suggestions.empty());

    REQUIRE(SendEvent(editor, ftx::Event::Tab));
    // Pre-fix, whole-line replacement produced "src/main.cpp" -- the prose was
    // gone AND the '/'-less content was a misroutable fragment. The '@' must
    // survive (the server's expansion scans for '@' + non-space tokens).
    CHECK(state.content == "please review @src/main.cpp");
    // Pre-fix, the cursor never moved off its pre-completion index (right
    // after "@src/ma"), landing mid-word instead of at the end of the
    // spliced completion.
    CHECK(state.cursor_position == static_cast<int>(state.content.size()));
}

TEST_CASE("prompt editor: Enter accepts the highlighted suggestion instead of submitting",
         "[prompt_editor][regression]") {
    // Before this fix, Enter always submitted state.content verbatim even
    // with the suggestion list open -- typing "/comp" toward "/compact",
    // seeing the list open, and pressing Enter (the natural "I've chosen
    // this" key) submitted the literal text "/comp" as a command, which is
    // not a real command name. Tab was the only accept key.
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    // CommandCatalogSource returns a REFERENCE (see its header comment): a
    // lambda returning a vector BY VALUE still compiles against that
    // signature (a prvalue converts to a const-ref parameter), but the
    // reference std::function's invoker hands back then dangles -- the
    // temporary is destroyed at the end of the wrapped call, before
    // command_catalog()'s caller ever reads it. The catalog needs its own
    // named storage, outliving `editor`'s use the same way `state` does.
    const std::vector<CommandCatalogEntry> catalog = {
        {"compact", "compact the history", false, false}};
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        [&catalog]() -> const std::vector<CommandCatalogEntry>& { return catalog; });

    for (const char c : std::string_view("/comp")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    // The command-catalog match runs at RENDER time (it must read content
    // AFTER Input applies each keystroke -- see MakePromptEditor's comment on
    // why the character handler itself would read one keystroke stale).
    RenderFrame(editor);
    REQUIRE(state.list_open);
    REQUIRE_FALSE(state.suggestions.empty());

    REQUIRE(SendEvent(editor, ftx::Event::Return));

    CHECK(commands.empty());   // never submitted the raw prefix
    CHECK(texts.empty());
    CHECK(state.content == "/compact ");   // accepted the completion instead
    CHECK(state.cursor_position == static_cast<int>(state.content.size()));
    CHECK_FALSE(state.list_open);

    // The list is now closed: a second Enter submits normally, unchanged.
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(commands.size() == 1);
    CHECK(commands[0] == "/compact");
}

TEST_CASE("prompt editor: Enter submits in ONE press when the user typed the "
          "whole command by hand", "[prompt_editor][regression]") {
    // Regression: with the list open (any prefix match keeps it open, even
    // an EXACT one), Enter always ran the "accept suggestion" branch first --
    // for a fully-typed "/help", that reinserted the identical text and
    // consumed the keystroke, so submitting required a SECOND Enter. This
    // typed the full name in one call to SendEvent (character by character),
    // then presses Enter exactly once.
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> commands;
    const std::vector<CommandCatalogEntry> catalog = {
        {"help", "list commands", true, false}, {"help2", "unrelated", true, false}};
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        [&catalog]() -> const std::vector<CommandCatalogEntry>& { return catalog; });

    for (const char c : std::string_view("/help")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    // Render once so the catalog match (render-time -- see MakePromptEditor's
    // comment) actually populates state.suggestions before Enter is sent;
    // otherwise this Enter would trivially fall through an EMPTY list rather
    // than exercising the "already typed the selection" fallthrough below.
    RenderFrame(editor);
    REQUIRE(state.list_open);   // "/help" is still a PREFIX of "/help2"
    REQUIRE_FALSE(state.suggestions.empty());

    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(commands.size() == 1);
    CHECK(commands[0] == "/help");
}

TEST_CASE("prompt editor: a bare '/' lists every command", "[prompt_editor][regression]") {
    // Regression: CommandPrefixOf("/") returns "" (empty prefix), and the
    // catalog-matching guard used to read "!prefix.empty()" -- so a bare '/'
    // (the single biggest discoverability affordance in the app) showed no
    // suggestions at all, indistinguishable from "not a command line".
    const auto theme = TestTheme();
    PromptEditorState state;
    const std::vector<CommandCatalogEntry> catalog = {{"help", "list commands", true, false},
                                                       {"compact", "compact history", false, false}};
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        [&catalog]() -> const std::vector<CommandCatalogEntry>& { return catalog; });

    REQUIRE(SendEvent(editor, ftx::Event::Character("/")));
    RenderFrame(editor);   // the catalog match runs at render time
    CHECK(state.list_open);
    REQUIRE(state.suggestions.size() == 2);
}

TEST_CASE("prompt editor: ctrl+a/e/u/k are readline-style line editing", "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; }, nullptr);

    state.content = "hello world";
    state.cursor_position = 5;   // between "hello" and " world"

    SECTION("ctrl+k kills from the cursor to the end") {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string(1, '\x0B'))));
        CHECK(state.content == "hello");
        CHECK(state.cursor_position == 5);
    }
    SECTION("ctrl+u kills from the start to the cursor") {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string(1, '\x15'))));
        CHECK(state.content == " world");
        CHECK(state.cursor_position == 0);
    }
    SECTION("ctrl+a moves the cursor to the start") {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string(1, '\x01'))));
        CHECK(state.content == "hello world");   // unchanged
        CHECK(state.cursor_position == 0);
    }
    SECTION("ctrl+e moves the cursor to the end") {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string(1, '\x05'))));
        CHECK(state.content == "hello world");   // unchanged
        CHECK(state.cursor_position == static_cast<int>(state.content.size()));
    }
}

TEST_CASE("prompt editor: the stale suggestion list is not completable mid-debounce") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string& prefix) {
            return std::vector<std::string>{prefix + "-result"};
        });

    for (const char c : std::string_view("@src")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    RenderFrame(editor);
    REQUIRE_FALSE(state.suggestions.empty());   // results for "@src" arrived

    // Keep typing: the re-arm clears the old list immediately. Pre-fix the old
    // entries stayed completable inside the 120 ms window -- Tab completed a
    // result for a prefix the user had already typed past.
    for (const char c : std::string_view("cli")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    CHECK(state.suggestions.empty());
    CHECK(state.completions.empty());
    CHECK(state.list_open);   // a fresh answer will re-open it
}

// ---------------------------------------------------------------------------
// Prompt history (up/down arrow recall)
// ---------------------------------------------------------------------------

namespace {
void Type(const ftxui::Component& editor, std::string_view text) {
    for (const char c : text) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
}
}  // namespace

TEST_CASE("prompt editor: up-arrow recalls the last submitted line, down-arrow "
         "restores the draft",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [](const std::string&) {}, [](const std::string&) { return std::vector<std::string>{}; });

    Type(editor, "first message");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(texts.size() == 1);
    CHECK(state.content.empty());

    // Start a second, unsubmitted line -- this is the "draft" down-arrow
    // must restore.
    Type(editor, "unsent draft");

    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "first message");

    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content == "unsent draft");   // the draft survives the round trip
    CHECK_FALSE(state.history_cursor.has_value());
}

TEST_CASE("prompt editor: up-arrow walks further back through older entries, "
         "stopping at the oldest",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    Type(editor, "one");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "two");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "three");
    REQUIRE(SendEvent(editor, ftx::Event::Return));

    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "three");
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "two");
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "one");
    // Already at the oldest: another up-arrow stays put, not out of bounds.
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "one");

    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content == "two");
    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content == "three");
    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content.empty());   // past the newest: back to the (empty) draft
}

TEST_CASE("prompt editor: a consecutive duplicate submission does not grow history",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    Type(editor, "same line");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "same line");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    CHECK(state.history.size() == 1);
}

TEST_CASE("prompt editor: typing while browsing history exits browse mode",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    Type(editor, "old entry");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    REQUIRE(state.history_cursor.has_value());

    REQUIRE(SendEvent(editor, ftx::Event::Character("!")));
    CHECK_FALSE(state.history_cursor.has_value());
    CHECK(state.content == "old entry!");   // the edit itself is preserved
}

TEST_CASE("prompt editor: deleting the whole command line closes the stale suggestion list",
          "[prompt_editor][regression]") {
    // Regression: Backspace/Delete were not edits as far as the editor was
    // concerned -- only is_character() re-armed the list. "/se" then three
    // backspaces left the "/se" suggestions rendered over an EMPTY input,
    // where the still-open list's Enter "accepted" a completion the user had
    // already erased. The render-time command block re-populates the list
    // the same frame when the remaining text is still a command line, so a
    // live list never flickers off.
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    const std::vector<CommandCatalogEntry> catalog = {
        {"send", "send a thing", false, false},
        {"session", "session stuff", false, false},
        {"set", "set stuff", false, false}};
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        [&catalog]() -> const std::vector<CommandCatalogEntry>& { return catalog; });

    Type(editor, "/se");
    RenderFrame(editor);
    REQUIRE(state.list_open);
    REQUIRE_FALSE(state.suggestions.empty());

    // Remove all using delete (backspace), exactly the reported repro.
    for (int i = 0; i < 3; ++i) REQUIRE(SendEvent(editor, ftx::Event::Backspace));
    RenderFrame(editor);
    CHECK(state.content.empty());
    CHECK_FALSE(state.list_open);   // the stale list is gone with its text
    CHECK(state.suggestions.empty());
    CHECK(state.completions.empty());

    // And the next submission is the plain text the user actually typed --
    // it must land as a chat turn, never as an autocomplete insertion.
    Type(editor, "hi");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(texts.size() == 1);
    CHECK(texts[0] == "hi");
    CHECK(commands.empty());
}

TEST_CASE("prompt editor: arrowing past the suggestion viewport scrolls the "
          "selection into view",
          "[prompt_editor][regression]") {
    // Regression: the suggestion list wrapped its content as
    // `size(HEIGHT, LESS_THAN, 8) | yframe`. In FTXUI 7 that ORDER breaks
    // focus-based scrolling: Size::SetBox clamps the content box to the cap
    // while the frame's requirement min_y shrinks to the cap too, so the
    // frame sees internal == external ("nothing to scroll") and everything
    // past the cap is silently clipped -- a broad "/" against a long catalog
    // showed only the head, and arrowing down stopped moving the view.
    // yframe-first (frame inside the cap) keeps the full height inside the
    // frame, so the selected row's `focus` anchor scrolls it into view.
    const auto theme = TestTheme();
    PromptEditorState state;
    const std::vector<CommandCatalogEntry> catalog = [] {
        std::vector<CommandCatalogEntry> entries;
        for (int i = 0; i < 15; ++i) {
            entries.push_back({"cmd" + std::to_string(i), "summary " + std::to_string(i), false,
                               false});
        }
        return entries;
    }();
    auto editor = MakePromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        [&catalog]() -> const std::vector<CommandCatalogEntry>& { return catalog; });

    REQUIRE(SendEvent(editor, ftx::Event::Character("/")));
    RenderFrame(editor);
    REQUIRE(state.list_open);
    REQUIRE(state.suggestions.size() == 15);

    // Move the selection well past the 8-row viewport.
    for (int i = 0; i < 10; ++i) REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    REQUIRE(state.suggestion_selected == 10);

    const std::string frame = RenderFrame(editor);
    CHECK(frame.find("cmd10") != std::string::npos);   // the selection is visible
    CHECK(frame.find("cmd0 ") == std::string::npos);   // the head scrolled away
}

// ---------------------------------------------------------------------------
// /help overlay (commands help view)
// ---------------------------------------------------------------------------

TEST_CASE("commands help overlay lists the whole catalog and closes on Escape",
          "[commands_help]") {
    // Regression: /help's server-side listing reached the transient notice
    // line, capped at a handful of rows with no way to scroll -- a catalog
    // larger than the viewport was unreachable. The overlay is built from
    // the client's ListCommands catalog, so the LAST entry below must be
    // rendered too.
    const auto theme = TestTheme();
    const std::vector<CommandCatalogEntry> catalog = [] {
        std::vector<CommandCatalogEntry> entries;
        for (int i = 0; i < 15; ++i) {
            entries.push_back({"cmd" + std::to_string(i), "summary " + std::to_string(i), false,
                               false});
        }
        return entries;
    }();
    bool closed = false;
    auto view = MakeCommandsHelpView(catalog, theme, [&] { closed = true; });

    const std::string frame = RenderFrame(view);
    CHECK(frame.find("cmd0") != std::string::npos);   // the head
    CHECK(frame.find("cmd14") != std::string::npos);  // ...and the tail, unreachable before

    REQUIRE(SendEvent(view, ftx::Event::Escape));
    CHECK(closed);
}
