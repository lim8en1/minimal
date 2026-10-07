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
#include <optional>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "ftxui_harness.hpp"
#include "md4c_engine.hpp"
#include "syntax_grammars.hpp"

#include "minimal/client/chat_model.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "prompt_editor_fixture.hpp"
#include "minimal/client/view/prompt_host.hpp"
#include "minimal/client/view/tools_panel.hpp"

using namespace minimal::client;
using namespace minimal_test;
using minimal::client::prompt_editor::PromptEditorState;
namespace ftx = ftxui;
namespace v1 = minimal::v1;

namespace {
// A CommandCatalogSource over a fixed catalog: one snapshot, same identity
// on every call (a real CommandCatalogCache swaps it only on a refresh).
minimal::client::CommandCatalogSource SnapshotOf(
    const std::vector<minimal::client::CommandCatalogEntry>& catalog) {
    auto snapshot = std::make_shared<const std::vector<minimal::client::CommandCatalogEntry>>(catalog);
    return [snapshot] { return snapshot; };
}

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

// Whether the row-cursor marker (on the row's FIRST element, i.e. its
// header line -- the row's own text sits on the line(s) after it) is
// immediately followed, within the row's own block, by `row_text`.
// The row cursor is the heavy gutter bar (chat_view.cpp's kCursorGutterGlyph):
// a shape, not just a color, so it is findable in a colorless render.
bool CursorMarkerPrecedes(const std::string& frame, std::string_view row_text) {
    const std::size_t marker = frame.find("┃");
    if (marker == std::string::npos) return false;
    const std::size_t window_end = std::min(frame.size(), marker + 200);
    return frame.find(row_text, marker) != std::string::npos &&
           frame.find(row_text, marker) < window_end;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ask modal (tool-ask plugin)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Prompt editor
// ---------------------------------------------------------------------------

TEST_CASE("prompt editor: Enter submits text, / lines go to the command path") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    auto editor = MakeTestPromptEditor(
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
    auto editor = MakeTestPromptEditor(
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
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    state.content = "before after";
    state.cursor_position = 6;   // between "before" and " after"
    REQUIRE(SendEvent(editor, ftx::Event::Character("\x1b\r")));   // Alt+Enter (ESC CR)

    CHECK(state.content == "before\n after");   // split where the cursor was, not appended
    CHECK(state.cursor_position == 7);           // advanced past the inserted newline
}

// Bracketed paste (DECSET 2004): the terminal-side app wraps a paste in
// \x1b[200~ ... \x1b[201~ and every line break inside it arrives as a plain
// Return. Those Returns used to hit the submit path -- each pasted line went
// out as its own turn. FTXUI 7 delivers the (unknown CSI) markers as
// Event::Special with the raw bytes preserved, so the editor tracks the mode
// and turns in-paste Returns into newlines.
TEST_CASE("prompt editor: a bracketed paste becomes one multi-line buffer, not one "
          "submit per line") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; });

    REQUIRE(SendEvent(editor, ftx::Event::Special("\x1b[200~")));   // paste start
    REQUIRE(SendEvent(editor, ftx::Event::Character("one")));
    REQUIRE(SendEvent(editor, ftx::Event::Return));   // in-paste: newline, NOT submit
    REQUIRE(SendEvent(editor, ftx::Event::Character("two")));
    REQUIRE(SendEvent(editor, ftx::Event::Special("\x1b[201~")));   // paste end
    CHECK(state.content == "one\ntwo");
    CHECK(texts.empty());   // nothing leaked out mid-paste

    REQUIRE(SendEvent(editor, ftx::Event::Return));   // after the paste: submits normally
    REQUIRE(texts.size() == 1);
    CHECK(texts[0] == "one\ntwo");
    CHECK(state.content.empty());
}

TEST_CASE("prompt editor: history recall places the cursor at the end of the recalled line") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakeTestPromptEditor(
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
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // Tab A, version 2. Then tab B, ALSO at version 2: the per-session
    // counter collides across tabs, so version alone cannot key the cache.
    const auto frame_a = RenderElement(view.Render(SnapshotWithText("session-a", 2, "alpha")));
    CHECK(frame_a.find("alpha") != std::string::npos);

    const auto frame_b = RenderElement(view.Render(SnapshotWithText("session-b", 2, "beta")));
    CHECK(frame_b.find("beta") != std::string::npos);   // fails pre-fix: stale "alpha"
    CHECK(frame_b.find("alpha") == std::string::npos);
}

// B3: RebuildRows caches ChatRowsFromMessages' own output on scrollback_page's
// pointer identity, so a rebuild triggered by a collapse-epoch bump ALONE
// (no new page) reuses it instead of re-walking the whole scrollback window.
// This proves the cache serves FRESH content on a genuine page swap for the
// SAME session (the risk a naive "keyed on session_id alone" cache would
// have gotten wrong), not just that it's fast.
TEST_CASE("chat view: a new scrollback page for the SAME session is not served stale "
         "from the row-materialization cache") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    const auto snap_v1 = SnapshotWithText("session-fresh", 1, "first page");
    const auto frame_1 = RenderElement(view.Render(snap_v1));
    CHECK(frame_1.find("first page") != std::string::npos);

    // A DIFFERENT scrollback_page shared_ptr (a real fetch would rebuild it
    // too -- state.cpp's ApplyScrollbackPage is the only mutation site),
    // same session, higher version: must render the NEW content, not a
    // cache hit keyed loosely enough to miss the page swap. seq 2, NOT
    // SnapshotWithText's fixed seq 1: a transcript's seq is server-assigned
    // and never reused (state.cpp), which is the invariant row_cache_ itself
    // relies on ("same-key rows from one message are stable") -- reusing
    // seq 1 here would hit THAT per-row Elements cache with v1's stale
    // render, a self-inflicted test bug rather than anything the
    // materialized-rows cache under test gets wrong.
    SessionState::Snapshot snap_v2 = snap_v1;
    snap_v2.transcript_version = 2;
    snap_v2.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::vector<v1::StoredMessage>{StoredUserText(2, "second page")});
    const auto frame_2 = RenderElement(view.Render(snap_v2));
    CHECK(frame_2.find("second page") != std::string::npos);
    CHECK(frame_2.find("first page") == std::string::npos);

    // Re-rendering the SAME (v2) snapshot again -- e.g. a collapse toggle
    // elsewhere bumping only collapse_epoch_ -- must still show v2's content:
    // the materialized-rows cache hit must not resurrect v1's rows.
    const auto frame_2_again = RenderElement(view.Render(snap_v2));
    CHECK(frame_2_again == frame_2);
}

// C2: a reader scrolled up mid-turn must not watch their own viewport slide
// toward the newest content as the model keeps streaming underneath. Anchors
// on scroll_from_bottom_ > 0 used to be measured as a distance from the
// literal end of `column` -- which includes the live streaming block and
// live tool rows, both of which grow every delta/tool event -- so the SAME
// scroll_from_bottom_ value pointed at a DIFFERENT row every time more
// content streamed in.
TEST_CASE("chat view: scrolling up mid-turn stays put while the streaming block grows") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // 20 persisted rows; scroll up far enough (15 of 20) that the anchored
    // row sits well clear of the live streaming tail in a short viewport.
    auto snapshot = SnapshotWithRows("session-drift", 1, 20);
    view.ScrollBy(15);

    // One live TEXT segment (track C4: what a real streaming turn's
    // SessionState::Snapshot carries -- live_segments, not just the flat
    // streaming_text convenience field ChatView no longer renders from
    // directly), fed through its own stream slot exactly like
    // TabMaintenance::Maintain does -- slot keyed by the segment's stable
    // id, the same key the production lookup uses.
    std::vector<LiveTextStreamSlot> streams(1);
    streams[0].segment_id = 1;
    const auto append_segment = [&](const std::string& text) {
        snapshot.live_segments = {LiveTurnSegment{
            .kind = LiveTurnSegment::Kind::Text, .segment_id = 1,
            .text_view = std::make_shared<const std::string>(text)}};
        if (!streams[0].view) streams[0].view = engine.MakeStream();
        streams[0].view->AppendDelta(std::string_view(text).substr(streams[0].seen));
        streams[0].seen = text.size();
    };

    append_segment("the reply so far");
    const auto before =
        RenderElement(view.Render(snapshot, streams), /*width=*/40, /*height=*/6);
    CHECK(before.find("row-19") == std::string::npos);   // scrolled well clear of the newest

    // More deltas land -- same persisted page (same transcript_version), just
    // a longer live tail, exactly what happens between two frames of one
    // streaming turn.
    append_segment("the reply so far, now with a great deal more text streamed in "
                   "since the last frame, several sentences longer than before");
    const auto after =
        RenderElement(view.Render(snapshot, streams), /*width=*/40, /*height=*/6);

    // The anchored (persisted) row is unaffected by how much grew below it --
    // pre-fix, this frame differed on every delta even though nothing the
    // user was actually looking at had changed.
    CHECK(after == before);
}

// C4: the live view renders this turn's text and tool activity INTERLEAVED,
// in the order SessionState recorded them -- not the whole streaming block
// first and every tool row after it regardless of when each actually
// happened. Asserted on relative POSITION in the rendered frame text
// (find() index order), the same "content, not visuals" contract this
// file's other tests already use.
TEST_CASE("chat view: live text and tool activity render interleaved, "
         "in arrival order", "[chat_view][live_segments]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "session-interleave";
    snap.transcript_version = 1;
    snap.live_segments = {
        LiveTurnSegment{.kind = LiveTurnSegment::Kind::Text, .segment_id = 1,
                        .text_view = std::make_shared<const std::string>("opening remarks")},
        LiveTurnSegment{.kind = LiveTurnSegment::Kind::Tool, .segment_id = 2,
                        .tool_use_id = "call-1"},
        LiveTurnSegment{.kind = LiveTurnSegment::Kind::Text, .segment_id = 3,
                        .text_view = std::make_shared<const std::string>("closing remarks")},
    };
    ToolExecutionState tool;
    tool.tool_use_id = "call-1";
    tool.tool_name = "search_the_archives";
    tool.started = true;
    snap.tools = {tool};

    std::vector<LiveTextStreamSlot> streams(3);
    streams[0].segment_id = 1;
    streams[0].view = engine.MakeStream();
    streams[0].view->AppendDelta("opening remarks");
    streams[0].seen = 15;
    streams[2].segment_id = 3;
    streams[2].view = engine.MakeStream();
    streams[2].view->AppendDelta("closing remarks");
    streams[2].seen = 15;

    const auto frame = RenderElement(view.Render(snap, streams));
    const auto opening_pos = frame.find("opening remarks");
    const auto tool_pos = frame.find("search_the_archives");
    const auto closing_pos = frame.find("closing remarks");
    REQUIRE(opening_pos != std::string::npos);
    REQUIRE(tool_pos != std::string::npos);
    REQUIRE(closing_pos != std::string::npos);
    // Pre-fix, BOTH text pieces rendered as one fused block ABOVE the tool
    // row unconditionally -- "closing remarks" would appear before, not
    // after, "search_the_archives".
    CHECK(opening_pos < tool_pos);
    CHECK(tool_pos < closing_pos);
}

namespace {
class ThrowingUserTextRenderer final : public plugins::MessageRenderer {
public:
    ftxui::Element Render(const ChatRow&, const Theme&,
                          const plugins::RowRenderContext&) const override {
        throw std::runtime_error("boom");
    }
};

class ThrowingStream final : public plugins::MarkdownEngine::Stream {
public:
    void AppendDelta(std::string_view) override {}
    void Finalize() override {}
    ftxui::Element Render(const Theme&) const override { throw std::runtime_error("boom"); }
};
}  // namespace

TEST_CASE("chat view: a throwing UserText renderer does not crash the echo row") {
    // Regression: the pending-turn echo is a SEPARATE call from RebuildRows'
    // own try/catch (this is the live echo, not a persisted row) -- it had
    // no containment of its own.
    const auto theme = TestTheme();
    plugins::MessageRendererRegistry messages;
    messages.SetAttribution("test");
    messages.Register(ChatRowKind::UserText, std::make_unique<ThrowingUserTextRenderer>());
    messages.ClearAttribution();
    plugins::ToolResultRendererRegistry tool_renderers;

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "session-echo-throws";
    snap.transcript_version = 1;
    snap.pending_user_text = "hello";

    // The point: this must not crash or propagate.
    const auto frame = RenderElement(view.Render(snap));
    CHECK(frame.find("hello") == std::string::npos);   // the throwing echo just didn't render
}

TEST_CASE("chat view: a throwing live markdown stream renders a fallback, not a crash") {
    const auto theme = TestTheme();
    plugins::MessageRendererRegistry messages;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::ToolResultRendererRegistry tool_renderers;

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "session-stream-throws";
    snap.transcript_version = 1;
    snap.live_segments = {
        LiveTurnSegment{.kind = LiveTurnSegment::Kind::Text, .segment_id = 1,
                        .text_view = std::make_shared<const std::string>("partial")},
    };
    std::vector<LiveTextStreamSlot> streams(1);
    streams[0].segment_id = 1;
    streams[0].view = std::make_unique<ThrowingStream>();

    const auto frame = RenderElement(view.Render(snap, streams));
    CHECK(frame.find("render failed") != std::string::npos);
}

TEST_CASE("chat view: has_earlier_messages renders a load-older hint") {
    // Regression: has_earlier_messages was never read by this file at all --
    // a resumed session with more history than the initial page looked like
    // its full history unless the user already knew about chat.load_older.
    const auto theme = TestTheme();
    plugins::MessageRendererRegistry messages;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::ToolResultRendererRegistry tool_renderers;

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "session-earlier";
    snap.transcript_version = 1;
    snap.has_earlier_messages = true;

    const auto frame = RenderElement(view.Render(snap));
    CHECK(frame.find("earlier messages") != std::string::npos);
}

namespace {

// StoredToolCallAndResult plus a display payload on the result (page[0] is
// the newest message, the result).
std::vector<v1::StoredMessage> StoredToolCallWithDisplay(const std::string& tool_name,
                                                         const std::string& result_text,
                                                         const nlohmann::json& display) {
    auto page = StoredToolCallAndResult(1, tool_name, result_text);
    page[0].mutable_message()->mutable_content(0)->mutable_tool_result()->set_display_json(
        display.dump());
    return page;
}

class LabelKindRenderer final : public plugins::DisplayRenderer {
public:
    std::string_view kind() const override { return "test_kind"; }
    ftxui::Element Render(const plugins::ToolResultView& result, const Theme&,
                          const plugins::RowRenderContext&) const override {
        return ftxui::text("KIND " + result.display.at("label").get<std::string>());
    }
};

class ThrowingKindRenderer final : public plugins::DisplayRenderer {
public:
    std::string_view kind() const override { return "test_kind"; }
    ftxui::Element Render(const plugins::ToolResultView&, const Theme&,
                          const plugins::RowRenderContext&) const override {
        throw std::runtime_error("malformed payload");
    }
};

class BashNameRenderer final : public plugins::ToolResultRenderer {
public:
    std::string_view tool_name() const override { return "bash"; }
    ftxui::Element Render(const plugins::ToolResultView&, const Theme&,
                          const plugins::RowRenderContext&) const override {
        return ftxui::text("NAME RENDERER");
    }
};

// The registries every display-dispatch test below needs, core defaults filled.
struct DisplayHarness {
    Theme theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::DisplayRendererRegistry display_renderers;

    DisplayHarness() {
        minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
        plugins::RegisterCoreMessageRenderers(messages);
        plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    }

    std::string Render(std::vector<v1::StoredMessage> page) {
        ChatView view(theme, messages, tool_renderers, display_renderers);
        SessionState::Snapshot snap;
        snap.session_id = "session-display";
        snap.transcript_version = 1;
        snap.scrollback_page =
            std::make_shared<const std::vector<v1::StoredMessage>>(std::move(page));
        return RenderElement(view.Render(snap));
    }
};

}  // namespace

TEST_CASE("chat view: a result's display kind picks its renderer; a tool-name renderer wins") {
    DisplayHarness h;
    h.display_renderers.Register(std::make_unique<LabelKindRenderer>());
    const auto page = StoredToolCallWithDisplay("bash", "plain text",
                                                {{"kind", "test_kind"}, {"label", "hello"}});

    SECTION("the kind renderer draws the row") {
        CHECK(h.Render(page).find("KIND hello") != std::string::npos);
    }
    SECTION("a renderer for the tool's NAME takes precedence over the kind") {
        h.tool_renderers.Register(std::make_unique<BashNameRenderer>());
        const auto frame = h.Render(page);
        CHECK(frame.find("NAME RENDERER") != std::string::npos);
        CHECK(frame.find("KIND hello") == std::string::npos);
    }
}

TEST_CASE("chat view: a throwing or unclaimed display kind falls back to the text row") {
    DisplayHarness h;
    h.display_renderers.Register(std::make_unique<ThrowingKindRenderer>());

    // The default text row's headline: status glyph + tool name.
    SECTION("throwing") {
        const auto frame = h.Render(
            StoredToolCallWithDisplay("bash", "plain text", {{"kind", "test_kind"}}));
        CHECK(frame.find("✓ bash") != std::string::npos);
    }
    SECTION("no renderer for the kind") {
        const auto frame = h.Render(
            StoredToolCallWithDisplay("bash", "plain text", {{"kind", "nobody_draws_this"}}));
        CHECK(frame.find("✓ bash") != std::string::npos);
    }
}

TEST_CASE("chat view: the core diff and process renderers draw their payloads") {
    DisplayHarness h;
    static const auto languages = minimal::client::syntax_grammars::Definitions();
    const minimal::client::SyntaxHighlighter highlighter(h.theme, languages);
    plugins::RegisterCoreDisplayRenderers(h.display_renderers, *h.engine_registry.get(),
                                          highlighter);

    SECTION("diff: expanded by default, with markers and counts") {
        const nlohmann::json display = {
            {"kind", "diff"},
            {"path", "a.txt"},
            {"created", false},
            {"hunks",
             {{{"old_start", 1},
               {"old_count", 1},
               {"new_start", 1},
               {"new_count", 1},
               {"lines",
                {{{"op", "-"}, {"text", "old line"}}, {{"op", "+"}, {"text", "new line"}}}}}}}};
        const auto frame =
            h.Render(StoredToolCallWithDisplay("edit", "replaced 1 occurrence", display));
        CHECK(frame.find("a.txt") != std::string::npos);
        CHECK(frame.find("+1") != std::string::npos);
        CHECK(frame.find("- old line") != std::string::npos);
        CHECK(frame.find("+ new line") != std::string::npos);
    }
    SECTION("process: the header carries the command, the status badge and the duration") {
        const nlohmann::json display = {{"kind", "process"},  {"command", "make"},
                                        {"exit_code", 2},     {"duration_ms", 1500},
                                        {"output", "boom\n"}, {"cancelled", false},
                                        {"timed_out", false}};
        const auto frame = h.Render(StoredToolCallWithDisplay("bash", "boom\n[exit status: 2]",
                                                              display));
        CHECK(frame.find("$ make") != std::string::npos);
        CHECK(frame.find("exit 2") != std::string::npos);
        CHECK(frame.find("1.5s") != std::string::npos);
    }
}

TEST_CASE("chat view: a completed tool call with no name-specific renderer still renders "
         "(falls back to the message-kind default)") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;   // deliberately empty: no plugin
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "session-tool";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "total 0\ndrwxr-xr-x  2 x  x  64 x x"));

    const auto frame = RenderElement(view.Render(snap));
    CHECK(frame.find("bash") != std::string::npos);

    // ToolActivity rows collapse by default (one-liner; expandable body) --
    // focus and expand this one to verify the fallback renderer's body
    // content, not just its header.
    view.MoveRowFocus(0);
    view.ToggleCollapseFocused();
    const auto expanded = RenderElement(view.Render(snap));
    CHECK(expanded.find("total 0") != std::string::npos);
}

TEST_CASE("chat view: a tool result with one early newline and a long single-line tail "
         "is capped from the END, not the START") {
    // The bug: the tail cap searched BACKWARD from the 4000-byte window's
    // start for a newline to resume from -- "exit\n" + a long single-line
    // tail has its only '\n' at index 4, so the backward search found it and
    // kept almost the WHOLE body (thousands of bytes), not ~4000. A forward
    // search from the window's start only ever lands AT OR AFTER it, so the
    // kept tail must end at END_MARKER and must NOT reach back to
    // START_MARKER, which sits right after the one early newline.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // Space-separated filler, not one giant unbroken token: paragraph() wraps
    // at word boundaries, so a single 6000-byte word would render as one
    // over-wide line clipped by the screen rather than wrapping down to
    // where END_MARKER could be asserted on.
    std::string filler;
    for (int i = 0; i < 1200; ++i) filler += "aaaaa ";
    const std::string result_text = "exit\nSTART_MARKER " + filler + "END_MARKER";

    SessionState::Snapshot snap;
    snap.session_id = "session-tool-tail";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", result_text));

    RenderElement(view.Render(snap));   // first pass: builds the row cache MoveRowFocus needs
    view.MoveRowFocus(0);
    view.ToggleCollapseFocused();
    const auto expanded = RenderElement(view.Render(snap), /*width=*/100, /*height=*/80);
    CHECK(expanded.find("END_MARKER") != std::string::npos);
    CHECK(expanded.find("START_MARKER") == std::string::npos);
}

TEST_CASE("chat view: ScrollBy moves the viewport off the pinned-to-newest default") {
    // Pre-fix, Render() always focus()ed the LAST row unconditionally, so no
    // key or mouse event could ever change what's visible -- this is the
    // regression test for that (arrow/mouse scrolling did nothing).
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-scroll", 1, 40);

    // A viewport far shorter than 40 rows: which slice is visible reflects
    // the scroll offset (offset 0 pins to the newest row; the assert is on
    // the visible slice, not an exact edge -- the rows are 3 lines each and
    // the 8-line viewport shows two and a bit).
    const auto pinned =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);
    CHECK(pinned.find("row-39") != std::string::npos);

    // Scroll toward older messages (positive == toward older), far enough
    // (30 of 40 rows) that an 8-line viewport cannot possibly show the same
    // slice: proves ScrollBy actually moves the anchor rather than a no-op.
    view.ScrollBy(30);
    const auto scrolled =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);
    CHECK(scrolled != pinned);
    CHECK(scrolled.find("row-39") == std::string::npos);

    // Scrolling back down by the same amount re-pins to the newest row,
    // reproducing the original (pre-scroll) view exactly.
    view.ScrollBy(-30);
    const auto repinned =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);
    CHECK(repinned == pinned);
}

TEST_CASE("chat view: ScrollToBottom resets the anchor directly, not just relatively") {
    // The scroll-position-drift UX fix: previously the only way back to the
    // newest row after scrolling up was walking down one row at a time (or
    // knowing exactly how far up you'd gone). ScrollToBottom() is bound to
    // chat.scroll_bottom (PageDown) and also called on every chat turn submit.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-scroll-bottom", 1, 40);

    const auto pinned =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);

    view.ScrollBy(30);   // scroll far up, same as the ScrollBy test above
    const auto scrolled =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);
    CHECK(scrolled != pinned);

    view.ScrollToBottom();   // one call, not "however far up ScrollBy went"
    const auto back =
        RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/8);
    CHECK(back == pinned);
    CHECK(back.find("row-39") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Scroll-anchor chunking (a long message is several anchors, not one)
// ---------------------------------------------------------------------------

// An assistant TEXT message (the AgentMarkdown row kind): the "mouse scrolls
// too fast" scenario's protagonist -- one long response used to be one
// indivisible scroll anchor.
v1::StoredMessage StoredAssistantText(int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_ASSISTANT);
    stored.mutable_message()->add_content()->mutable_text()->set_text(text);
    return stored;
}

TEST_CASE("agent markdown: a long response renders as multiple scroll-anchor chunks",
          "[chat_view][scroll_anchors]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    const auto* markdown = messages.Find(ChatRowKind::AgentMarkdown);
    REQUIRE(markdown != nullptr);

    plugins::RowRenderContext ctx;
    ChatRow row;
    row.kind = ChatRowKind::AgentMarkdown;

    // 25 blank-line-separated paragraphs: one chunk each, and every chunk
    // carries the full text through to the render -- splitting must not lose
    // or reorder content.
    std::string prose;
    for (int i = 0; i < 25; ++i) {
        if (i > 0) prose += "\n";
        prose += "para-" + std::to_string(i) + " some body text";
    }
    row.text = prose;
    const auto prose_blocks = markdown->RenderBlocks(row, theme, ctx);
    CHECK(prose_blocks.size() >= 2);
    std::string joined;
    for (const auto& block : prose_blocks) joined += RenderElement(block);
    for (int i = 0; i < 25; ++i) {
        CHECK(joined.find("para-" + std::to_string(i)) != std::string::npos);
    }
    // The speaker label rides the FIRST chunk only.
    CHECK(RenderElement(prose_blocks.front()).find("assistant") != std::string::npos);
    CHECK(RenderElement(prose_blocks[1]).find("assistant") == std::string::npos);

    // A fence longer than the cap splits too; each piece is re-fenced so the
    // split point never turns code into prose, and every line survives.
    std::string code = "```cpp\ncode-start\n";
    for (int i = 0; i < 58; ++i) code += "int v" + std::to_string(100 + i) + ";\n";
    code += "code-end\n```";
    row.text = code;
    const auto code_blocks = markdown->RenderBlocks(row, theme, ctx);
    CHECK(code_blocks.size() >= 3);
    std::string joined_code;
    for (const auto& block : code_blocks) joined_code += RenderElement(block);
    CHECK(joined_code.find("code-start") != std::string::npos);
    CHECK(joined_code.find("code-end") != std::string::npos);
    for (int i = 0; i < 58; ++i) {
        CHECK(joined_code.find("v" + std::to_string(100 + i)) != std::string::npos);
    }
}

TEST_CASE("chat view: scrolling moves within one tall message, not just between messages",
          "[chat_view][scroll_anchors]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // ONE message of 30 distinct single-screen lines. Scrolling is measured
    // in SCREEN LINES (ScrollRegion), so the viewport always moves through
    // the message's interior -- pre-line-scrolling this transcript was one
    // focus anchor (whole-message snaps) and then one CHUNK anchor (leaps).
    std::string text;
    for (int i = 0; i < 30; ++i) {
        if (i > 0) text += "\n";
        text += "chunk-point-" + std::to_string(100 + i);
    }
    SessionState::Snapshot snap;
    snap.session_id = "tall";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::vector<v1::StoredMessage>{StoredAssistantText(1, text)});

    const auto pinned = RenderElement(view.Render(snap), /*width=*/40, /*height=*/8);
    CHECK(pinned.find("chunk-point-129") != std::string::npos);

    // One keyboard step = half the 8-line viewport = 4 lines: the view moves
    // a small, readable distance INSIDE the message, off the newest content.
    view.ScrollBy(1);
    const auto stepped = RenderElement(view.Render(snap), /*width=*/40, /*height=*/8);
    CHECK(stepped != pinned);
    CHECK(stepped.find("chunk-point-125") != std::string::npos);
    CHECK(stepped.find("chunk-point-129") == std::string::npos);

    // Many steps clamp at the message's very top -- no overshoot to burn.
    view.ScrollBy(20);
    const auto scrolled = RenderElement(view.Render(snap), /*width=*/40, /*height=*/8);
    CHECK(scrolled != pinned);
    CHECK(scrolled.find("chunk-point-129") == std::string::npos);
    CHECK(scrolled.find("chunk-point-10") != std::string::npos);

    // And the way back is exact.
    view.ScrollToBottom();
    const auto repinned =
        RenderElement(view.Render(snap), /*width=*/40, /*height=*/8);
    CHECK(repinned == pinned);
}

TEST_CASE("chat view: ScrollWheelBy is time-gated, keyboard ScrollBy is not",
          "[chat_view][scroll_anchors]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-wheel-gate", 1, 40);

    view.ScrollWheelBy(1);
    CHECK(view.IsScrolledAway());

    view.ScrollToBottom();
    CHECK_FALSE(view.IsScrolledAway());
    // The second event lands within the gate (this test executes far faster
    // than the gate's 30ms): dropped, so the view stays pinned. Trackpad
    // momentum is the flood this bounds.
    view.ScrollWheelBy(1);
    CHECK_FALSE(view.IsScrolledAway());

    // The keyboard path bypasses the gate -- key repeat is already
    // rate-limited by the OS.
    view.ScrollBy(1);
    CHECK(view.IsScrolledAway());
}

// ---------------------------------------------------------------------------
// Render() assembly cache (the per-row loop is redone only when something
// that affects it changes, not on every call)
// ---------------------------------------------------------------------------

TEST_CASE("chat view: the row-cursor highlight moves after a render with no transcript change",
          "[chat_view]") {
    // The bug an under-invalidated assembly cache would reintroduce: moving
    // the row cursor (MoveRowFocus) changes no transcript_version/session_id/
    // collapse_epoch, so a cache keyed only on those would keep showing the
    // marker on the OLD row forever.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-cursor-move", 1, 5);

    RenderElement(view.Render(snapshot));   // first pass: builds row_keys_ for MoveRowFocus
    view.MoveRowFocus(0);                   // starts at the newest row (row-4)
    const auto at_newest = RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/20);
    CHECK(CursorMarkerPrecedes(at_newest, "row-4"));
    CHECK_FALSE(CursorMarkerPrecedes(at_newest, "row-0"));

    view.MoveRowFocus(4);   // toward older rows: row-4 -> row-0
    const auto at_oldest = RenderElement(view.Render(snapshot), /*width=*/40, /*height=*/20);
    CHECK(at_oldest != at_newest);
    CHECK(CursorMarkerPrecedes(at_oldest, "row-0"));
    CHECK_FALSE(CursorMarkerPrecedes(at_oldest, "row-4"));
}

TEST_CASE("chat view: toggling timestamps updates the render with no transcript change",
          "[chat_view]") {
    // Same class of bug as the row-cursor test above: ToggleTimestamps()
    // changes no transcript_version/session_id/collapse_epoch either.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    v1::StoredMessage stored = StoredUserText(1, "hello");
    stored.set_created_at_unix_ms(1700000000000LL);
    SessionState::Snapshot snap;
    snap.session_id = "session-timestamps";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::vector<v1::StoredMessage>{stored});

    const std::string stamp = FormatClockTime(1700000000000LL);
    REQUIRE_FALSE(stamp.empty());
    const auto with_stamp = RenderElement(view.Render(snap));
    CHECK(with_stamp.find(stamp) != std::string::npos);

    view.ToggleTimestamps();
    const auto without_stamp = RenderElement(view.Render(snap));
    CHECK(without_stamp.find(stamp) == std::string::npos);
}

TEST_CASE("chat view: a growing thinking segment renders its full text, not a stale "
         "cached prefix",
          "[chat_view]") {
    // paragraph() over a live Thinking segment is cached by text equality
    // (see chat_view.cpp) -- a genuinely grown text must be a cache MISS,
    // not silently keep showing the shorter, earlier version.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    SessionState::Snapshot snap;
    snap.session_id = "session-thinking-grow";
    snap.transcript_version = 1;
    LiveTurnSegment segment;
    segment.kind = LiveTurnSegment::Kind::Thinking;
    // Snapshots carry the segment's copy-on-publish view (state.hpp
    // LiveTurnSegment); the working `text` field is SessionState-internal.
    segment.text_view = std::make_shared<const std::string>("short reasoning");
    snap.live_segments = {segment};

    const auto first = RenderElement(view.Render(snap), /*width=*/80, /*height=*/10);
    CHECK(first.find("short reasoning") != std::string::npos);

    // A republished (grown) view must be a cache MISS, not silently keep
    // showing the shorter, earlier version.
    snap.live_segments[0].text_view =
        std::make_shared<const std::string>(
            "short reasoning plus a lot more reasoning appended afterward");
    const auto second = RenderElement(view.Render(snap), /*width=*/80, /*height=*/10);
    CHECK(second.find("plus a lot more reasoning appended afterward") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Mouse-click collapse (ChatView::ToggleCollapseAt)
// ---------------------------------------------------------------------------

v1::StoredMessage StoredThinking(int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_ASSISTANT);
    stored.mutable_message()->add_content()->mutable_thinking()->set_text(text);
    return stored;
}

TEST_CASE("chat view: clicking a thinking row toggles its collapse", "[chat_view][click_collapse]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // One user text row (seq 1) and one thinking row (seq 2, the newest --
    // pages are DESCENDING by seq). Thinking collapses BY DEFAULT.
    SessionState::Snapshot snap;
    snap.session_id = "clicky";
    snap.transcript_version = 1;
    snap.scrollback_page =
        std::make_shared<const std::vector<v1::StoredMessage>>(std::vector<v1::StoredMessage>{
            StoredThinking(2, "pondering deeply"), StoredUserText(1, "hello human"),
        });

    const auto collapsed =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
    CHECK(collapsed.find("chars") != std::string::npos);   // "thinking  N chars"
    CHECK(collapsed.find("pondering deeply") == std::string::npos);
    // The click affordance (the disclosure glyph) marks the collapsed header.
    CHECK(collapsed.find("▸") != std::string::npos);

    // Click the thinking header's screen row: the row expands.
    const int header_y = ScreenLineOf(collapsed, "thinking");
    REQUIRE(header_y >= 0);
    CHECK(view.ToggleCollapseAt(header_y));

    const auto expanded =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
    CHECK(expanded.find("pondering deeply") != std::string::npos);
    CHECK(expanded.find("▸") == std::string::npos);
    // The affordance flips with the state.
    CHECK(expanded.find("▾") != std::string::npos);

    // Clicking again -- re-located, the expanded body moved the header --
    // collapses it back.
    const int header_y2 = ScreenLineOf(expanded, "thinking");
    REQUIRE(header_y2 >= 0);
    CHECK(view.ToggleCollapseAt(header_y2));
    const auto recollapsed =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
    CHECK(recollapsed.find("pondering deeply") == std::string::npos);
    CHECK(recollapsed.find("chars") != std::string::npos);
}

TEST_CASE("chat view: clicks that miss a collapsible row are not consumed",
          "[chat_view][click_collapse]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    // A user text row is NOT click-collapsible; the filler above the
    // bottom-aligned short transcript belongs to no row at all.
    const auto snap = SnapshotWithText("missy", 1, "hello human");
    const auto frame =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);

    const int row_y = ScreenLineOf(frame, "hello human");
    REQUIRE(row_y >= 0);
    CHECK_FALSE(view.ToggleCollapseAt(row_y));
    CHECK_FALSE(view.ToggleCollapseAt(0));   // the filler: no row under it
    CHECK_FALSE(view.ToggleCollapseAt(-5));  // off-screen: nothing to hit
}

TEST_CASE("chat view: clicking a tool-activity row toggles its collapse",
          "[chat_view][click_collapse]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());

    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);

    SessionState::Snapshot snap;
    snap.session_id = "clicky-tool";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "ls output line"));

    // Tool rows collapse BY DEFAULT (CollapsedByDefault).
    const auto collapsed =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
    CHECK(collapsed.find("ls output line") == std::string::npos);
    CHECK(collapsed.find("▸") != std::string::npos);   // the disclosure glyph

    const int header_y = ScreenLineOf(collapsed, "✓ bash");
    REQUIRE(header_y >= 0);
    CHECK(view.ToggleCollapseAt(header_y));

    const auto expanded =
        RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
    CHECK(expanded.find("ls output line") != std::string::npos);
    CHECK(expanded.find("▾") != std::string::npos);
}

TEST_CASE("chat view: ui.json fold defaults set the startup collapse tier",
          "[chat_view][fold]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    plugins::DisplayRendererRegistry display_renderers;

    SessionState::Snapshot snap;
    snap.session_id = "fold-defaults";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "ls output line"));

    // kFoldExpanded: the tool row STARTS expanded with no toggle at all.
    {
        ChatView view(theme, messages, tool_renderers, display_renderers);
        view.SetFoldDefaults(kFoldDefaultAuto, kFoldExpanded);
        const auto rendered = RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
        CHECK(rendered.find("ls output line") != std::string::npos);
    }
    // kFoldCollapsed: the ask_user expanded exception is OVERridden by the
    // explicit user choice.
    {
        ChatView view(theme, messages, tool_renderers, display_renderers);
        view.SetFoldDefaults(kFoldDefaultAuto, kFoldCollapsed);
        const auto rendered = RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
        CHECK(rendered.find("ls output line") == std::string::npos);
        CHECK(rendered.find("▸") != std::string::npos);
    }
    // kFoldDefaultAuto: the built-in default stands (tool rows collapsed).
    {
        ChatView view(theme, messages, tool_renderers, display_renderers);
        const auto rendered = RenderElement(view.Render(snap), /*width=*/60, /*height=*/24);
        CHECK(rendered.find("ls output line") == std::string::npos);
    }
}

TEST_CASE("chat view: the kind fold toggles flip the tier and yield to per-row "
          "and collapse-all choices",
          "[chat_view][fold]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    plugins::DisplayRendererRegistry display_renderers;

    SessionState::Snapshot snap;
    snap.session_id = "fold-toggle";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "ls output line"));

    ChatView view(theme, messages, tool_renderers, display_renderers);
    // Tool rows are collapsed by the built-in default, so the first toggle
    // expands the kind.
    CHECK(view.ToggleToolFold() == kFoldExpanded);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") !=
          std::string::npos);
    // ...and the second re-collapses it (never back to auto in-session).
    CHECK(view.ToggleToolFold() == kFoldCollapsed);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") ==
          std::string::npos);

    // A per-row override WINS over the kind default (most specific tier).
    view.ToggleToolFold();                       // kind: expanded again
    const int header_y =
        ScreenLineOf(RenderElement(view.Render(snap), 60, 24), "\u2713 bash");
    REQUIRE(header_y >= 0);
    CHECK(view.ToggleCollapseAt(header_y));      // per-row: collapsed
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") ==
          std::string::npos);

    // A kind toggle clears per-row overrides (the newer intent wins).
    CHECK(view.ToggleToolFold() == kFoldCollapsed);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") ==
          std::string::npos);
}

TEST_CASE("chat view: a kind fold toggle clears the global collapse choice",
          "[chat_view][fold]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    plugins::DisplayRendererRegistry display_renderers;

    SessionState::Snapshot snap;
    snap.session_id = "fold-vs-all";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        StoredToolCallAndResult(1, "bash", "ls output line"));

    ChatView view(theme, messages, tool_renderers, display_renderers);
    // First ToggleCollapseAll flips the global choice to COLLAPSED
    // (IsCollapsed returns it verbatim): every row, tool rows included,
    // hides.
    view.ToggleCollapseAll();
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") ==
          std::string::npos);
    // The kind toggle flips from what the kind currently SHOWS (globally
    // collapsed) to expanded -- and clears the global choice, which is the
    // only way the row can actually appear.
    CHECK(view.ToggleToolFold() == kFoldExpanded);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") !=
          std::string::npos);
    // And the next toggle re-collapses the kind (never back to auto).
    CHECK(view.ToggleToolFold() == kFoldCollapsed);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("ls output line") ==
          std::string::npos);
}

TEST_CASE("chat view: thinking fold toggle flips thinking rows only",
          "[chat_view][fold]") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.get());
    plugins::DisplayRendererRegistry display_renderers;

    // One thinking row persisted (the wire shape chat_model.cpp's
    // block.has_thinking() branch turns into a Thinking row).
    v1::StoredMessage stored;
    stored.set_seq(1);
    stored.mutable_message()->set_role(v1::ROLE_ASSISTANT);
    stored.mutable_message()->add_content()->mutable_thinking()->set_text(
        "pondering the fold");
    const std::vector<v1::StoredMessage> page{std::move(stored)};

    SessionState::Snapshot snap;
    snap.session_id = "fold-thinking";
    snap.transcript_version = 1;
    snap.scrollback_page =
        std::make_shared<const std::vector<v1::StoredMessage>>(std::move(page));

    ChatView view(theme, messages, tool_renderers, display_renderers);
    // Thinking is collapsed by default, so the toggle expands the kind.
    CHECK(view.ToggleThinkingFold() == kFoldExpanded);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("pondering the fold") !=
          std::string::npos);
    CHECK(view.ToggleThinkingFold() == kFoldCollapsed);
    CHECK(RenderElement(view.Render(snap), 60, 24).find("pondering the fold") ==
          std::string::npos);
}

// ---------------------------------------------------------------------------
// @-mention completion (R5)
// ---------------------------------------------------------------------------

TEST_CASE("prompt editor: Tab splices the mention token, preserving the prose") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{"src/main.cpp"}; });

    // Type prose plus a partial mention.
    for (const char c : std::string_view("please review @src/ma")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    // The debounce arms at Render (the match is derived there, after Input
    // applied the keystrokes) and fires from a LATER render once the window
    // has elapsed.
    RenderFrame(editor);                              // arms the debounce
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    RenderFrame(editor);                              // the gate fires
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

// C5: accept_suggestion used to splice over content.rfind('@') -- the LAST
// '@' anywhere in the buffer -- regardless of where the cursor actually was.
// Editing an EARLIER, unfinished mention (cursor moved back into it, e.g. to
// fix a typo) completed the WRONG token and destroyed everything typed after
// the cursor, including a second, later '@' mention.
TEST_CASE("prompt editor: Tab completes the mention AT THE CURSOR, not the "
         "last '@' in the buffer", "[prompt_editor][regression]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; });

    // "compare @fo and @bar.txt", cursor placed right after the FIRST,
    // unfinished mention -- a second '@' mention sits further along the
    // line, past the cursor. Driven directly through `state` (not
    // SendEvent's character-by-character typing, which only ever leaves the
    // cursor at the end) -- exactly the "user pressed Left to fix an
    // earlier mention" scenario, which the debounce/character-handler path
    // isn't what's under test here; the SPLICE is.
    state.content = "compare @fo and @bar.txt";
    state.cursor_position = static_cast<int>(std::string("compare @fo").size());
    state.list_open = true;
    state.suggestions = {"src/foo.cpp"};
    state.completions = {"src/foo.cpp"};
    state.suggestion_selected = 0;

    REQUIRE(SendEvent(editor, ftx::Event::Tab));

    // The token AT THE CURSOR is completed; the prose and the SECOND mention
    // after the cursor survive untouched. Pre-fix this produced
    // "compare @fo and @src/foo.cpp" (completed the wrong, LAST '@') or
    // worse, depending on which '@' rfind found.
    CHECK(state.content == "compare @src/foo.cpp and @bar.txt");
    CHECK(state.cursor_position ==
         static_cast<int>(std::string("compare @src/foo.cpp").size()));
}

// C6: the render-time command-catalog rebuild skips its catalog scan when
// (prefix, session) matches the last computed key -- safe only because it
// ALSO checks state.suggestions still holds what THIS closure last wrote
// there. accept_suggestion (Tab) clears state.suggestions out from under
// it; backspacing back to the SAME prefix afterward must not serve a stale
// "nothing changed" cache hit over that now-empty list.
TEST_CASE("prompt editor: suggestions repopulate after accepting then returning "
         "to the same prefix", "[prompt_editor][regression]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    std::vector<std::string> texts;
    std::vector<std::string> commands;
    const std::vector<CommandCatalogEntry> catalog = {
        {"compact", "compact the history", false, false}};
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

    for (const char c : std::string_view("/compact")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    RenderFrame(editor);
    REQUIRE_FALSE(state.suggestions.empty());

    REQUIRE(SendEvent(editor, ftx::Event::Tab));   // accepts -> "/compact ", clears suggestions
    CHECK(state.content == "/compact ");
    CHECK(state.suggestions.empty());

    // Back to the EXACT prefix ("compact") the cache last saw -- a cache
    // keyed on (prefix, session) alone would wrongly report a hit here and
    // leave state.suggestions empty (and the list effectively unusable)
    // instead of repopulating it.
    REQUIRE(SendEvent(editor, ftx::Event::Backspace));
    CHECK(state.content == "/compact");
    RenderFrame(editor);
    CHECK_FALSE(state.suggestions.empty());
}

TEST_CASE("prompt editor: a refreshed catalog snapshot updates the list without retyping",
         "[prompt_editor][regression]") {
    // The catalog is pulled in the background (CommandCatalogCache): a
    // command file added mid-run lands as a NEW snapshot. The per-frame
    // cache keyed on (prefix, session) alone would keep serving the old
    // list until the prefix changed; the snapshot's identity is in the key.
    const auto theme = TestTheme();
    PromptEditorState state;
    auto current = std::make_shared<const std::vector<CommandCatalogEntry>>(
        std::vector<CommandCatalogEntry>{{"deploy", "old", false, true}});
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        [&current]() { return current; });

    for (const char c : std::string_view("/de")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    RenderFrame(editor);
    REQUIRE(state.suggestions.size() == 1);

    current = std::make_shared<const std::vector<CommandCatalogEntry>>(
        std::vector<CommandCatalogEntry>{{"deploy", "old", false, true},
                                         {"describe", "new", false, true}});
    RenderFrame(editor);   // same prefix, new snapshot
    CHECK(state.suggestions.size() == 2);
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
    const std::vector<CommandCatalogEntry> catalog = {
        {"compact", "compact the history", false, false}};
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

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
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

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
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

    REQUIRE(SendEvent(editor, ftx::Event::Character("/")));
    RenderFrame(editor);   // the catalog match runs at render time
    CHECK(state.list_open);
    REQUIRE(state.suggestions.size() == 2);
}

TEST_CASE("prompt editor: ctrl+a/e/u/k are readline-style line editing", "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    auto editor = MakeTestPromptEditor(
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
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string& prefix) {
            return std::vector<std::string>{prefix + "-result"};
        });

    for (const char c : std::string_view("@src")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    // Arming happens at Render (see the Tab-splice test above): one render
    // derives the prefix, a later one -- after the debounce window -- fires.
    RenderFrame(editor);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    RenderFrame(editor);
    REQUIRE_FALSE(state.suggestions.empty());   // results for "@src" arrived

    // Keep typing: the render-time derivation clears the old list as soon as
    // the prefix moves past it. Pre-fix the old entries stayed completable
    // inside the 120 ms window -- Tab completed a result for a prefix the
    // user had already typed past.
    for (const char c : std::string_view("cli")) {
        REQUIRE(SendEvent(editor, ftx::Event::Character(std::string{c})));
    }
    RenderFrame(editor);   // the derivation sees the new prefix and re-arms
    CHECK(state.suggestions.empty());
    CHECK(state.completions.empty());
    CHECK_FALSE(state.list_open);   // a fresh answer re-opens it
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
    PromptHistory hist;   // explicit: the assertions below read the timeline
    std::vector<std::string> texts;
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [](const std::string&) {}, [](const std::string&) { return std::vector<std::string>{}; },
        nullptr, nullptr, nullptr, nullptr, &hist);

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
    PromptHistory hist;   // explicit: the assertions below read the timeline
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        nullptr, nullptr, nullptr, nullptr, &hist);

    Type(editor, "one");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "two");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "three");
    REQUIRE(SendEvent(editor, ftx::Event::Return));

    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "three");
    // A RENDER happens between keystrokes in the real app. The recalled
    // lines here are not commands, so the command gate never opens a list;
    // the plain walk must be unaffected by the history-owns-the-arrows rule.
    // Rendering between the arrows proves it at the same tier.
    REQUIRE(minimal_test::RenderFrame(editor).find("three") != std::string::npos);
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

TEST_CASE("prompt editor: history recall walks THROUGH a recalled command line "
          "whose suggestion list opened",
          "[prompt_editor][regression]") {
    // THE REPORTED BUG: submit /pause, ArrowUp again -- the recalled "/pause"
    // line makes the command gate (render pass) open the suggestion list, and
    // the list-navigation branches used to eat every further arrow (they ran
    // above the recall arms behind a !list_open guard). History could not be
    // walked past the first /-leading entry. Needs the interleaved RENDER:
    // list_open is a render-gate write, so an event-only harness never sees
    // the interception (why the suite stayed green).
    const auto theme = TestTheme();
    PromptEditorState state;
    PromptHistory hist;
    std::vector<std::string> texts;
    const std::vector<CommandCatalogEntry> catalog = {
        {"pause", "pause the session", true, false}};
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog),
        nullptr, nullptr, nullptr, &hist);

    Type(editor, "plain message");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "/pause");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    CHECK(hist.Entries().size() == 2);

    // Recall lands on "/pause", the next frame opens the command list.
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "/pause");
    RenderFrame(editor);
    REQUIRE(state.list_open);
    REQUIRE_FALSE(state.suggestions.empty());

    // The list is open, but the user is BROWSING: Up keeps walking the
    // timeline instead of moving the list's selection.
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.content == "plain message");
    CHECK(state.suggestion_selected == 0);   // the list was not navigated
    CHECK(hist.Entries()[*state.history_cursor - hist.FirstSeq()] == "plain message");

    // Down returns to "/pause" (still browsing), then further down past the
    // newest restores the draft -- the full walk stays intact.
    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content == "/pause");
    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.content.empty());
    CHECK_FALSE(state.history_cursor.has_value());
}

TEST_CASE("prompt editor: an open suggestion list still owns the arrows while "
          "typing a command name", "[prompt_editor]") {
    // The other half of the contract: history-owns-the-arrows applies ONLY
    // while browsing (state.history_cursor set). Typing /pa fresh opens the list and
    // Up/Down must still navigate it -- this is what the /-autocomplete is
    // FOR -- and Enter still accepts the highlighted entry.
    const auto theme = TestTheme();
    PromptEditorState state;
    PromptHistory hist;
    std::vector<std::string> commands;
    const std::vector<CommandCatalogEntry> catalog = {
        {"pause", "pause the session", true, false},
        {"perm", "show permissions", true, false}};
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {},
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog),
        nullptr, nullptr, nullptr, &hist);

    Type(editor, "seed");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "/p");
    RenderFrame(editor);
    REQUIRE(state.list_open);
    REQUIRE(state.suggestions.size() == 2);
    CHECK(state.suggestion_selected == 0);

    REQUIRE(SendEvent(editor, ftx::Event::ArrowDown));
    CHECK(state.suggestion_selected == 1);      // list navigates: not browsing
    CHECK_FALSE(state.history_cursor.has_value());
    REQUIRE(SendEvent(editor, ftx::Event::ArrowUp));
    CHECK(state.suggestion_selected == 0);

    REQUIRE(SendEvent(editor, ftx::Event::Tab));   // accept -> "/pause "
    CHECK(state.content == "/pause ");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    REQUIRE(commands.size() == 1);                 // submitted as a command
    CHECK(commands.back() == "/pause");
}

TEST_CASE("prompt editor: up-arrow inside a multi-line draft does not recall "
         "history",
         "[prompt_editor]") {
    // Regression: the recall branch had no check for a multi-line
    // in-progress draft (built with Alt+Enter) -- Up used to unconditionally
    // recall the last history entry over it instead of moving the cursor.
    const auto theme = TestTheme();
    PromptEditorState state;
    PromptHistory hist;   // explicit: the assertions below read the timeline
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        nullptr, nullptr, nullptr, nullptr, &hist);

    Type(editor, "first message");
    REQUIRE(SendEvent(editor, ftx::Event::Return));   // seed history

    Type(editor, "line one");
    REQUIRE(SendEvent(editor, ftx::Event::Character("\x1b\r")));   // Alt+Enter: newline
    Type(editor, "line two");
    REQUIRE(state.content == "line one\nline two");

    SendEvent(editor, ftx::Event::ArrowUp);
    CHECK(state.content == "line one\nline two");   // draft untouched, not recalled
    CHECK_FALSE(state.history_cursor.has_value());
}

TEST_CASE("prompt editor: a consecutive duplicate submission does not grow history",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    PromptHistory hist;   // explicit: the assertions below read the timeline
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        nullptr, nullptr, nullptr, nullptr, &hist);

    Type(editor, "same line");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    Type(editor, "same line");
    REQUIRE(SendEvent(editor, ftx::Event::Return));
    CHECK(hist.Entries().size() == 1);
}

TEST_CASE("prompt editor: typing while browsing history exits browse mode",
         "[prompt_editor]") {
    const auto theme = TestTheme();
    PromptEditorState state;
    PromptHistory hist;   // explicit: the assertions below read the timeline
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        nullptr, nullptr, nullptr, nullptr, &hist);

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
    auto editor = MakeTestPromptEditor(
        theme, state, [&](const std::string& t) { texts.push_back(t); },
        [&](const std::string& c) { commands.push_back(c); },
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

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
    auto editor = MakeTestPromptEditor(
        theme, state, [](const std::string&) {}, [](const std::string&) {},
        [](const std::string&) { return std::vector<std::string>{}; },
        SnapshotOf(catalog));

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
// Running-tools panel
// ---------------------------------------------------------------------------

TEST_CASE("tools panel: the yframe scrolls to the newest tool, not stuck at the top",
          "[tools_panel]") {
    // The bug: RenderToolsPanel never called focus() on anything, so the
    // yframe frame_renderer.cpp wraps it in had no descendant to auto-scroll
    // to -- it stayed pinned at offset 0 (the header and the first tool)
    // forever, and later tools never became visible.
    const auto theme = TestTheme();
    AppState::TabSnapshot tab;
    tab.tab_id = "tab-1";
    tab.display_name = "session";
    for (int i = 0; i < 10; ++i) {
        ToolExecutionState tool;
        tool.tool_use_id = "t-" + std::to_string(i);
        tool.tool_name = "tool" + std::to_string(i);
        tool.started = true;
        tab.tools.push_back(std::move(tool));
    }
    const std::vector<AppState::TabSnapshot> tabs = {tab};

    // A viewport far shorter than the 11 rows (1 header + 10 tools) this
    // produces.
    const auto frame = RenderElement(
        RenderToolsPanel(tabs, /*stdout_buffer_lines=*/5, theme) | ftx::yframe,
        /*width=*/40, /*height=*/6);
    CHECK(frame.find("tool9") != std::string::npos);
    CHECK(frame.find("tool0") == std::string::npos);
}

// ============================================================================
// Row windowing (chat_model.hpp's SelectRowWindow + ChatView's span
// bookkeeping). The windowed path only engages from the SECOND render on --
// the first frame full-assembles and measures -- so every test here renders
// at least twice.
// ============================================================================

// A windowed frame elides rows whose spans sit outside the viewport: the
// frame shows the elision marker and the newest rows, never a row from the
// far end of the transcript. (The pin-to-newest default, 40 three-line rows
// in a 10-line viewport.)
TEST_CASE("chat view: windowing elides off-viewport rows and marks the cut") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);
    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-window", 1, 40);

    (void)RenderElement(view.Render(snapshot), 40, 10);   // full: measures
    const auto windowed = RenderElement(view.Render(snapshot), 40, 10);
    CHECK(windowed.find("row-39") != std::string::npos);
    CHECK(windowed.find("row-0") == std::string::npos);
    // The elision marker lives at the CUT POINT in content coordinates -- it
    // scrolls with the transcript and is only visible parked near it (the
    // scrolled-up resize test below exercises that end). What must hold
    // while pinned: the elided geometry reproduces the full tree's content
    // height, so the scroll accounting (applied offset 0) is unchanged.
    CHECK(view.scroll_state_for_test()->content_height ==
          121);   // separator + 40 rows x (2 lines + 1 spacer)
    CHECK(view.scroll_state_for_test()->applied_offset_from_bottom == 0);

    // Parked mid-transcript the viewport has elided rows on BOTH sides: the
    // cut is visible on the vscroll indicator (the head/tail fillers keep the
    // thumb on the full transcript's track), and neither transcript end may
    // leak in.
    view.ScrollBy(15);
    const auto parked = RenderElement(view.Render(snapshot), 40, 10);
    CHECK(parked.find("row-39") == std::string::npos);
    CHECK(parked.find("row-0") == std::string::npos);
    CHECK(view.scroll_state_for_test()->content_height == 121);
}

// THE RESIZE CONTRACT: growing or shrinking the terminal must not break the
// scroll accounting -- a pinned viewport keeps showing the newest row, a
// scrolled-up viewport keeps showing the rows it was parked on, and the
// span bookkeeping (which the resize invalidates only in screen terms, never
// in content terms) converges within a frame.
TEST_CASE("chat view: windowing survives resize while pinned to the newest row") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);
    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-resize-pin", 1, 40);

    // Tall -> short -> tall, pinned throughout: the newest row must be
    // visible in every frame, whatever the viewport.
    for (const int height : {24, 8, 16, 8, 24}) {
        const auto frame = RenderElement(view.Render(snapshot), 80, height);
        CAPTURE(height);
        CHECK(frame.find("row-39") != std::string::npos);
        CHECK(view.scroll_state_for_test()->applied_offset_from_bottom == 0);
    }
}

TEST_CASE("chat view: windowing survives resize while scrolled up") {
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    auto& engine = *engine_registry.get();
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::RegisterCoreMessageRenderers(messages);
    plugins::RegisterCoreAgentMarkdownRenderer(messages, engine);
    plugins::DisplayRendererRegistry display_renderers;
    ChatView view(theme, messages, tool_renderers, display_renderers);
    const auto snapshot = SnapshotWithRows("session-resize-scroll", 1, 40);

    (void)RenderElement(view.Render(snapshot), 80, 16);   // full: measures
    // Park on an early row, then resize around: the parked row stays on
    // screen (the offset-from-bottom is clamped against the measured
    // content, and content height is preserved by the fillers), and the
    // newest row never leaks in from below the fold.
    view.ScrollBy(30);
    for (const int height : {16, 8, 24, 12, 16}) {
        const auto frame = RenderElement(view.Render(snapshot), 80, height);
        CAPTURE(height);
        CHECK(frame.find("row-39") == std::string::npos);
        CHECK(frame.find("row-") != std::string::npos);   // some transcript row renders
    }
    // Scrolling back down re-pins and the newest row returns.
    view.ScrollToBottom();
    const auto back = RenderElement(view.Render(snapshot), 80, 16);
    CHECK(back.find("row-39") != std::string::npos);
}
