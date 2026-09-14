// The chat view's data plane: message-kind categorization, the command-notice
// routing, and the scrollback cursor. Pure -- the FTXUI rendering on top of
// this is deliberately not unit-tested (ARCHITECTURE.md "Testing").
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <type_traits>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/scrollback_refresh.hpp"

using namespace minimal;
using minimal::client::ChatRowKind;

namespace {

v1::StoredMessage MakeMessage(v1::Role role, std::vector<v1::ContentBlock> blocks,
                              int64_t seq = 0) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(role);
    for (auto& block : blocks) {
        *stored.mutable_message()->add_content() = std::move(block);
    }
    return stored;
}

v1::ContentBlock TextBlock(std::string text) {
    v1::ContentBlock block;
    block.mutable_text()->set_text(std::move(text));
    return block;
}

v1::ContentBlock ToolUse(const std::string& id, const std::string& name) {
    v1::ContentBlock block;
    block.mutable_tool_use()->set_id(id);
    block.mutable_tool_use()->set_name(name);
    return block;
}

v1::ContentBlock ToolResult(const std::string& id, const std::string& text, bool is_error) {
    v1::ContentBlock block;
    block.mutable_tool_result()->set_tool_use_id(id);
    block.mutable_tool_result()->set_is_error(is_error);
    block.mutable_tool_result()->add_content()->mutable_text()->set_text(text);
    return block;
}

v1::StoredMessage Attributed(v1::StoredMessage stored, const std::string& agent,
                             const std::string& model) {
    stored.set_agent_preset(agent);
    stored.set_model(model);
    return stored;
}

}  // namespace

TEST_CASE("AssistantLabel shows agent (short model) and degrades per missing half",
          "[chat_model][attribution]") {
    CHECK(client::AssistantLabel("build", "anthropic/sonnet-4-5") == "build (sonnet-4-5)");
    CHECK(client::AssistantLabel("build", "openrouter/anthropic/claude-opus-5") ==
          "build (claude-opus-5)");
    CHECK(client::AssistantLabel("build", "") == "build");
    CHECK(client::AssistantLabel("", "openai/gpt-5") == "gpt-5");
    CHECK(client::AssistantLabel("", "") == "assistant");
}

TEST_CASE("assistant rows carry their message's attribution; user rows do not",
          "[chat_model][attribution]") {
    // Descending, as GetMessages returns it.
    const std::vector<v1::StoredMessage> page = {
        Attributed(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("hi")}, 1), "build",
                   "anthropic/sonnet-4-5"),
        MakeMessage(v1::ROLE_USER, {TextBlock("hello")}, 0),
    };
    const auto rows = client::ChatRowsFromMessages(page);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == ChatRowKind::UserText);
    CHECK(rows[0].agent.empty());
    CHECK(rows[1].kind == ChatRowKind::AgentMarkdown);
    CHECK(rows[1].agent == "build");
    CHECK(rows[1].model == "anthropic/sonnet-4-5");
}

TEST_CASE("a switch divider marks where the agent or model changed",
          "[chat_model][attribution]") {
    const std::vector<v1::StoredMessage> page = {
        Attributed(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("c")}, 5), "plan", "openai/gpt-5"),
        MakeMessage(v1::ROLE_USER, {TextBlock("q3")}, 4),
        Attributed(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("b")}, 3), "build",
                   "anthropic/sonnet-4-5"),
        MakeMessage(v1::ROLE_USER, {TextBlock("q2")}, 2),
        Attributed(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("a")}, 1), "build",
                   "anthropic/sonnet-4-5"),
        MakeMessage(v1::ROLE_USER, {TextBlock("q1")}, 0),
    };
    const auto rows = client::ChatRowsFromMessages(page);
    // q1, a, q2, b (same speaker: no divider), q3, [switch], c
    REQUIRE(rows.size() == 7);
    CHECK(rows[3].kind == ChatRowKind::AgentMarkdown);
    CHECK(rows[4].kind == ChatRowKind::UserText);
    REQUIRE(rows[5].kind == ChatRowKind::Switch);
    CHECK(rows[5].switch_from_agent == "build");
    CHECK(rows[5].switch_from_model == "anthropic/sonnet-4-5");
    CHECK(rows[5].agent == "plan");
    CHECK(rows[5].model == "openai/gpt-5");
    CHECK(rows[5].seq == 5);   // the message where the change took effect
    CHECK(rows[6].kind == ChatRowKind::AgentMarkdown);
    CHECK_FALSE(client::CollapsedByDefault(rows[5]));
}

TEST_CASE("unknown attribution never draws a switch divider", "[chat_model][attribution]") {
    // Pre-attribution history followed by labeled rows: that is history
    // getting a label, not a switch.
    const std::vector<v1::StoredMessage> page = {
        Attributed(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("new")}, 3), "build",
                   "anthropic/sonnet-4-5"),
        MakeMessage(v1::ROLE_USER, {TextBlock("q2")}, 2),
        MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("old")}, 1),
        MakeMessage(v1::ROLE_USER, {TextBlock("q1")}, 0),
    };
    const auto rows = client::ChatRowsFromMessages(page);
    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) CHECK(row.kind != ChatRowKind::Switch);
    CHECK(rows[1].agent.empty());   // the old row stays unlabeled
}

TEST_CASE("LabelContinuations labels an assistant turn once, resetting at boundaries",
          "[chat_model][labels]") {
    using client::ChatRow;
    const auto row = [](ChatRowKind kind) { return ChatRow{.kind = kind}; };
    const std::vector<ChatRow> rows = {
        row(ChatRowKind::UserText),        // 0
        row(ChatRowKind::AgentMarkdown),   // 1 labeled
        row(ChatRowKind::ToolActivity),    // 2
        row(ChatRowKind::Thinking),        // 3
        row(ChatRowKind::AgentMarkdown),   // 4 continuation
        row(ChatRowKind::Switch),          // 5 boundary
        row(ChatRowKind::AgentMarkdown),   // 6 labeled again (new speaker)
        row(ChatRowKind::System),          // 7 boundary
        row(ChatRowKind::AgentMarkdown),   // 8 labeled
        row(ChatRowKind::UserText),        // 9
        row(ChatRowKind::AgentMarkdown),   // 10 labeled
    };
    const auto continuations = client::LabelContinuations(rows);
    REQUIRE(continuations.size() == rows.size());
    const std::vector<bool> expected = {false, false, false, false, true, false,
                                        false, false, false, false, false};
    CHECK(continuations == expected);
}

TEST_CASE("SummarizeToolInput prefers bash's $command over a generic dump",
          "[chat_model][tool_summary]") {
    nlohmann::json input = {{"command", "ls -la"}};
    CHECK(client::SummarizeToolInput("bash", input) == "$ ls -la");
}

TEST_CASE("SummarizeToolInput prefers ask_user's question text over a generic dump",
          "[chat_model][tool_summary]") {
    nlohmann::json input = nlohmann::json::parse(R"({"questions":[{"question":"which branch?"}]})");
    CHECK(client::SummarizeToolInput(std::string(client::kAskUserToolName), input) ==
         "which branch?");
}

TEST_CASE("SummarizeToolInput falls back to a truncated JSON dump for other tools",
          "[chat_model][tool_summary]") {
    nlohmann::json input = {{"path", "a.md"}};
    CHECK(client::SummarizeToolInput("read", input) == input.dump());
}

TEST_CASE("SummarizeToolInput is empty for null/empty input, not a bare \"{}\"",
          "[chat_model][tool_summary]") {
    CHECK(client::SummarizeToolInput("bash", nlohmann::json::object()).empty());
    CHECK(client::SummarizeToolInput("bash", nlohmann::json()).empty());
}

TEST_CASE("a plain conversation renders user text and agent markdown",
          "[chat_model]") {
    // Descending seq, as GetMessages returns: assistant LAST (seq 1), user
    // FIRST (seq 0). Display order is the reverse.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("**hello**")}));
    page.push_back(MakeMessage(v1::ROLE_USER, {TextBlock("hi there")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == ChatRowKind::UserText);
    // Verbatim, the markdown left alone: a user's typed underscores are not
    // emphasis. The VIEW chooses the styling; the row only records the kind.
    CHECK(rows[0].text == "hi there");
    CHECK(rows[1].kind == ChatRowKind::AgentMarkdown);
    CHECK(rows[1].text == "**hello**");
}

TEST_CASE("@-mention attachment blocks become attachment rows, text stays verbatim",
          "[chat_model][mention]") {
    // The E3 expansion shape: verbatim block 0, framed blocks after.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(
        v1::ROLE_USER,
        {TextBlock("see @a.md"), TextBlock("<file path=\"a.md\">\nA\n</file>"),
         TextBlock("<file-tree path=\"src/\">\nmain.cpp\n</file-tree>")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == ChatRowKind::UserText);
    CHECK(rows[1].kind == ChatRowKind::UserAttachment);
    CHECK(rows[1].attachment_path == "a.md");
    CHECK(rows[2].kind == ChatRowKind::UserAttachment);
    CHECK(rows[2].attachment_path == "src/");
}

TEST_CASE("a tool call whose result is in the page renders ONCE, with name and body",
          "[chat_model]") {
    // Descending: the RESULT is newer than its use (the use ran first), so it
    // comes first in the page. The use used to push a SECOND, empty row --
    // the "tool bash" with nothing under it, once per call.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "stdout tail", false)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 1);
    // The result row is the authoritative rendering: the call's name (which
    // rides on the USE) joined with the output body.
    CHECK(rows[0].kind == ChatRowKind::ToolActivity);
    CHECK(rows[0].tool_name == "bash");
    CHECK(rows[0].text == "stdout tail");
    CHECK_FALSE(rows[0].tool_is_error);
    CHECK_FALSE(rows[0].tool_pending);
}

TEST_CASE("a tool_use with no result in the page stays, flagged pending",
          "[chat_model]") {
    // An in-flight call (or a window boundary that cut between the pair):
    // dropping the row entirely would hide a call the model made, so it
    // renders as pending instead.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ChatRowKind::ToolActivity);
    CHECK(rows[0].tool_name == "bash");
    CHECK(rows[0].text.empty());
    CHECK(rows[0].tool_pending);
}

TEST_CASE("a tool result's display_json becomes the row's tool_display; malformed is dropped",
          "[chat_model][display]") {
    auto result = ToolResult("t1", "replaced 1 occurrence", false);
    result.mutable_tool_result()->set_display_json(R"({"kind":"diff","path":"a.cpp","hunks":[]})");
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {result}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "edit")}));

    auto rows = client::ChatRowsFromMessages(page);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].tool_display.at("kind") == "diff");
    CHECK(rows[0].tool_display.at("path") == "a.cpp");
    // A diff is the review surface for a change: it starts expanded, unlike
    // other tool rows.
    CHECK_FALSE(client::CollapsedByDefault(rows[0]));

    page[0].mutable_message()->mutable_content(0)->mutable_tool_result()->set_display_json(
        "{not json");
    rows = client::ChatRowsFromMessages(page);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].tool_display.is_null());
    CHECK(rows[0].text == "replaced 1 occurrence");   // the row itself survives
    CHECK(client::CollapsedByDefault(rows[0]));
}

TEST_CASE("an errored tool result is marked, not hidden", "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "boom", true)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].tool_is_error);
}

TEST_CASE("a subagent completion injection is a System row, not tool activity",
          "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "child text", false)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "spawn_subagent")}));

    const auto rows = client::ChatRowsFromMessages(page);

    // The result is the harness speaking, and with the duplicate use row gone
    // the System row IS the spawn's rendering.
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ChatRowKind::System);
    CHECK(rows[0].text == "child text");
    // The injection stays a REAL persisted message -- the model must see it at
    // the turn boundary (EXTENSIBILITY §4.4). Only its presentation is special.
}

TEST_CASE("thinking blocks become thinking rows", "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT,
                               {[] {
                                    v1::ContentBlock b;
                                    b.mutable_thinking()->set_text("pondering");
                                    return b;
                                }()}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ChatRowKind::Thinking);
    CHECK(rows[0].text == "pondering");
}

TEST_CASE("an empty page yields no rows", "[chat_model]") {
    CHECK(client::ChatRowsFromMessages({}).empty());
}

// Regression: every branch of the per-message dispatch used to `continue`
// past the seq-stamping loop at the bottom, so only the plain-text-user
// branch (the sole case with no early exit) ever stamped its rows -- every
// assistant, tool-result and harness-injection row kept the -1 "synthetic"
// sentinel. ChatView's render cache is keyed on (seq, occurrence-within-seq)
// (view/chat_view.cpp), so those rows all collapsed into one bucket keyed by
// POSITION instead of by message identity: paging in an older page shifts
// every position, and the cache then serves a stale render under the wrong
// message. One page exercising every branch: assistant text, a tool use +
// its result, and a spawn-subagent completion injection.
TEST_CASE("every row carries its source message's seq, whatever branch produced it",
          "[chat_model][regression]") {
    std::vector<v1::StoredMessage> page;
    // Descending by seq, as GetMessages returns.
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("spawn1", "child text", false)},
                               /*seq=*/40));
    page.push_back(
        MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("spawn1", "spawn_subagent")}, /*seq=*/30));
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "stdout tail", false)},
                               /*seq=*/20));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}, /*seq=*/10));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("hello")}, /*seq=*/0));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 3);
    // Display order (oldest first): text(0), bash result(20) -- the use's row
    // is deduped into it -- then the spawn's System row(40).
    CHECK(rows[0].kind == ChatRowKind::AgentMarkdown);
    CHECK(rows[0].seq == 0);
    CHECK(rows[1].kind == ChatRowKind::ToolActivity);
    CHECK(rows[1].seq == 20);
    CHECK(rows[1].tool_name == "bash");
    CHECK(rows[2].kind == ChatRowKind::System);
    CHECK(rows[2].seq == 40);
    for (const auto& row : rows) CHECK(row.seq >= 0);   // none left at the -1 sentinel
}

// --- command results: never chat -------------------------------------------

TEST_CASE("a command result becomes a notice with the right error flag",
          "[chat_model][command]") {
    v1::CommandResult ok;
    ok.set_status(v1::COMMAND_STATUS_OK);
    ok.set_message("agent: build");
    const auto ok_notice = client::NoticeFromCommandResult(ok);
    CHECK_FALSE(ok_notice.is_error);
    CHECK(ok_notice.message == "agent: build");

    v1::CommandResult bad;
    bad.set_status(v1::COMMAND_STATUS_ERROR);
    bad.set_message("usage: /agent <name>");
    CHECK(client::NoticeFromCommandResult(bad).is_error);
}

// The §4.4 invariant, expressed in the type system rather than by a check:
// a CommandNotice has NO conversion to the chat model's row type, so no future
// edit can "render a command result as a chat row" without defeating an
// explicit static_assert. Proven load-bearing by construction: adding such a
// conversion fails this compilation, which is the test.
static_assert(!std::is_convertible_v<client::CommandNotice, client::ChatRow>);
static_assert(!std::is_constructible_v<client::ChatRow, client::CommandNotice>);

// --- scrollback cursor ------------------------------------------------------

TEST_CASE("nothing loaded yet: fetch the newest page, cursorless", "[chat_model][scrollback]") {
    CHECK_FALSE(client::NextScrollbackCursor(-1, /*has_earlier=*/true).has_value());
}

TEST_CASE("history remains: the cursor is the oldest seq loaded", "[chat_model][scrollback]") {
    const auto cursor = client::NextScrollbackCursor(41, /*has_earlier=*/true);
    REQUIRE(cursor.has_value());
    CHECK(*cursor == 41);
}

TEST_CASE("history exhausted: no next page", "[chat_model][scrollback]") {
    CHECK_FALSE(client::NextScrollbackCursor(41, /*has_earlier=*/false).has_value());
}

// --- newest-slice refresh decision (AdvanceScrollbackRefresh) ----------------

namespace {

using client::ScrollbackRefreshAction;
using client::ScrollbackRefreshState;
using Snapshot = client::SessionState::Snapshot;

Snapshot SnapAt(uint64_t version, uint64_t refresh_seq) {
    Snapshot snap;
    snap.transcript_version = version;
    snap.refresh_seq = refresh_seq;
    snap.refresh_succeeded = true;
    return snap;
}

}  // namespace

TEST_CASE("a fresh tab fetches the newest slice on its first frame",
          "[chat_model][refresh]") {
    ScrollbackRefreshState state;   // rendered_version: nullopt
    const auto snap = SnapAt(0, 0);
    CHECK(AdvanceScrollbackRefresh(state, snap) == ScrollbackRefreshAction::kSubmit);
    CHECK(state.refresh_pending);
    CHECK(state.refresh_seq_seen == 0);
    CHECK(state.refresh_from_version == 0);
}

TEST_CASE("an in-flight refresh is not resubmitted", "[chat_model][refresh]") {
    ScrollbackRefreshState state;
    (void)AdvanceScrollbackRefresh(state, SnapAt(0, 0));
    // Same frame later (refresh_seq unmoved): nothing to do.
    CHECK(AdvanceScrollbackRefresh(state, SnapAt(0, 0)) == ScrollbackRefreshAction::kNone);
    CHECK(state.refresh_pending);
}

TEST_CASE("a resolved SUCCESSFUL empty page marks current instead of retrying forever",
          "[chat_model][refresh]") {
    // THE 20 Hz LOOP: a fresh tab's initial fetch resolves with no rows (the
    // transcript is empty), moving no version. Marking current on success --
    // not only on a version change -- is what stops the every-frame
    // GetMessages retry.
    ScrollbackRefreshState state;
    (void)AdvanceScrollbackRefresh(state, SnapAt(0, 0));
    // No new content, but the request resolved successfully.
    CHECK(AdvanceScrollbackRefresh(state, SnapAt(0, 1)) == ScrollbackRefreshAction::kNone);
    CHECK_FALSE(state.refresh_pending);
    REQUIRE(state.rendered_version.has_value());
    CHECK(*state.rendered_version == 0);
    // And a settled tab stays settled.
    CHECK(AdvanceScrollbackRefresh(state, SnapAt(0, 1)) == ScrollbackRefreshAction::kNone);
}

TEST_CASE("a resolved FAILED fetch leaves the mismatch so the same frame retries",
          "[chat_model][refresh]") {
    ScrollbackRefreshState state;
    (void)AdvanceScrollbackRefresh(state, SnapAt(0, 0));
    auto failed = SnapAt(0, 1);
    failed.refresh_succeeded = false;
    CHECK(AdvanceScrollbackRefresh(state, failed) == ScrollbackRefreshAction::kSubmit);
    CHECK(state.refresh_pending);   // resubmitted immediately
    CHECK(!state.rendered_version.has_value());
}

TEST_CASE("a turn-end version bump fetches even though streaming text is still held",
          "[chat_model][refresh]") {
    // THE MISSING-PROMPT BUG: the accumulated streaming text is cleared only
    // BY the applying refresh, so gating the turn-end fetch on "text empty"
    // consumed the version bump with a latch and no fetch ever fired. The
    // turn boundary -- Snapshot::turn_ended -- is what unlocks it.
    ScrollbackRefreshState state;
    state.rendered_version = 4;
    auto snap = SnapAt(5, 0);
    snap.has_streaming_text = true;
    snap.turn_ended = true;
    CHECK(AdvanceScrollbackRefresh(state, snap) == ScrollbackRefreshAction::kSubmit);
    CHECK(state.refresh_from_version == 5);
}

TEST_CASE("a version bump mid-turn with live streaming text fetches anyway",
          "[chat_model][refresh]") {
    // A tool round finishing bumps the version WHILE the turn keeps
    // streaming, and the fetch is safe mid-turn by construction:
    // ApplyScrollbackPage's content-matched retirement swaps in exactly the
    // persisted rows that cover the finished rounds' live segments, leaving
    // anything still open or running live. (The old latch -- "the streaming
    // block IS the content until the turn ends" -- was what made live tool
    // rows structurally different from their persisted twins.)
    ScrollbackRefreshState state;
    state.rendered_version = 4;
    auto snap = SnapAt(5, 0);
    snap.has_streaming_text = true;
    snap.turn_ended = false;
    CHECK(AdvanceScrollbackRefresh(state, snap) == ScrollbackRefreshAction::kSubmit);
    CHECK(state.refresh_pending);
    CHECK(state.refresh_from_version == 5);
}

TEST_CASE("empty streaming text fetches regardless of the turn flag",
          "[chat_model][refresh]") {
    // The idle path (resync also clears the accumulation mid-turn): nothing
    // live on screen, a version mismatch must reach a fetch.
    ScrollbackRefreshState state;
    state.rendered_version = 4;
    auto snap = SnapAt(5, 0);
    snap.turn_ended = false;
    CHECK(AdvanceScrollbackRefresh(state, snap) == ScrollbackRefreshAction::kSubmit);
}

TEST_CASE("a page that applied during a successful refresh marks the applied version",
          "[chat_model][refresh]") {
    ScrollbackRefreshState state;
    (void)AdvanceScrollbackRefresh(state, SnapAt(4, 0));
    // The worker applied a page (version moved 4 -> 5) and resolved.
    CHECK(AdvanceScrollbackRefresh(state, SnapAt(5, 1)) == ScrollbackRefreshAction::kNone);
    REQUIRE(state.rendered_version.has_value());
    CHECK(*state.rendered_version == 5);
}
