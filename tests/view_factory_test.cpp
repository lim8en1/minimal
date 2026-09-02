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

SessionState::Snapshot SnapshotWithText(const std::string& session_id, uint64_t version,
                                        const std::string& text) {
    SessionState::Snapshot snap;
    snap.session_id = session_id;
    snap.transcript_version = version;
    snap.scrollback_page = {StoredUserText(1, text)};
    return snap;
}

}  // namespace

// ---------------------------------------------------------------------------
// Session picker (R1b-2)
// ---------------------------------------------------------------------------

TEST_CASE("session picker: Enter resumes the HIGHLIGHTED row, not row 0") {
    const auto theme = TestTheme();
    const std::vector<v1::SessionSummary> sessions = {
        MakeSummary("s0", "alpha", 1), MakeSummary("s1", "beta", 2),
        MakeSummary("s2", "gamma", 3)};

    std::vector<std::string> resumed;
    std::vector<std::string> deleted;
    int closes = 0;
    SessionPickerCallbacks callbacks;
    callbacks.on_resume = [&](const std::string& id) { resumed.push_back(id); };
    callbacks.on_delete = [&](const std::string& id) { deleted.push_back(id); };
    callbacks.on_close = [&] { ++closes; };

    auto picker = MakeSessionPicker(sessions, theme, callbacks);

    const auto frame = RenderFrame(picker);
    CHECK(frame.find("alpha") != std::string::npos);
    CHECK(frame.find("gamma") != std::string::npos);

    // Down once -> row 1; Enter must resume THAT row. The pre-fix code froze
    // the CatchEvent's copy of `selected` at 0.
    REQUIRE(SendEvent(picker, ftx::Event::ArrowDown));
    REQUIRE(SendEvent(picker, ftx::Event::Return));
    REQUIRE(resumed.size() == 1);
    CHECK(resumed[0] == "s1");

    // The picker is still alive and stateful after resuming: delete nominates
    // the highlighted row too (the confirmation modal is RunTui's).
    REQUIRE(SendEvent(picker, ftx::Event::Character("d")));
    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0] == "s1");

    REQUIRE(SendEvent(picker, ftx::Event::Escape));
    CHECK(closes == 1);
}

TEST_CASE("session picker: empty list consumes Enter and Escape without nominating") {
    const auto theme = TestTheme();
    int nominations = 0;
    SessionPickerCallbacks callbacks;
    callbacks.on_resume = [&](const std::string&) { ++nominations; };
    callbacks.on_delete = [&](const std::string&) { ++nominations; };
    callbacks.on_close = [] {};

    auto picker = MakeSessionPicker({}, theme, callbacks);
    CHECK(RenderFrame(picker).find("no stored sessions") != std::string::npos);
    REQUIRE(SendEvent(picker, ftx::Event::Return));
    REQUIRE(SendEvent(picker, ftx::Event::Delete));
    CHECK(nominations == 0);
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
    auto editor = MakePromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        [] {
            return std::vector<CommandCatalogEntry>{{"compact", "compact the history", false,
                                                      false}};
        });

    for (const char c : std::string_view("/comp")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    REQUIRE(state.list_open);
    REQUIRE_FALSE(state.suggestions.empty());

    REQUIRE(SendEvent(editor, ftx::Event::Return));

    CHECK(commands.empty());   // never submitted the raw prefix
    CHECK(texts.empty());
    CHECK(state.content == "/compact ");   // accepted the completion instead
    CHECK_FALSE(state.list_open);

    // The list is now closed: a second Enter submits normally, unchanged.
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(commands.size() == 1);
    CHECK(commands[0] == "/compact");
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
