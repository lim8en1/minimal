// SessionState rules that had no test before the component split (state.hpp:
// PendingPrompts / ToolRunTracker / ScrollbackWindow / LiveTurnBuffer). Each
// pins CURRENT behaviour, so a behaviour-neutral extraction has to keep it --
// including one asymmetry recorded as-is (a thinking delta does not clear a
// pending prompt; a text delta does).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "minimal/client/state.hpp"

using namespace minimal::client;
namespace v1 = minimal::v1;

namespace {

v1::ToolExecutionEvent StartTool(const std::string& id) {
    v1::ToolExecutionEvent evt;
    evt.set_tool_use_id(id);
    evt.set_tool_name("bash");
    evt.set_started(true);
    return evt;
}

v1::ToolExecutionEvent FinishTool() {
    v1::ToolExecutionEvent evt;
    evt.set_started(false);
    return evt;
}

v1::BackgroundTaskEvent Background(const std::string& id, v1::BackgroundTaskEvent::State state) {
    v1::BackgroundTaskEvent evt;
    evt.set_tool_use_id(id);
    evt.set_state(state);
    evt.set_command("make");
    return evt;
}

// A page row: `seq`, a role, and at most one block of each kind given.
struct Row {
    std::int64_t seq;
    v1::Role role = v1::ROLE_USER;
    std::string text;
    std::string thinking;
    std::string tool_result_for;
};

v1::GetMessagesResponse Page(const std::vector<Row>& rows, bool has_more = false) {
    v1::GetMessagesResponse resp;
    for (const auto& row : rows) {
        auto* stored = resp.add_messages();
        stored->set_seq(row.seq);
        stored->mutable_message()->set_role(row.role);
        if (!row.text.empty()) {
            stored->mutable_message()->add_content()->mutable_text()->set_text(row.text);
        }
        if (!row.thinking.empty()) {
            stored->mutable_message()->add_content()->mutable_thinking()->set_text(row.thinking);
        }
        if (!row.tool_result_for.empty()) {
            stored->mutable_message()->add_content()->mutable_tool_result()->set_tool_use_id(
                row.tool_result_for);
        }
    }
    resp.set_has_more(has_more);
    return resp;
}

std::unique_ptr<PendingPermission> Permission(const std::string& request_id) {
    auto req = std::make_unique<PendingPermission>();
    req->request_id = request_id;
    return req;
}

}  // namespace

// --- turn opening -------------------------------------------------------------

TEST_CASE("a tool call can open a turn: turn_seq, both flat buffers, a sticky Error",
          "[session_state][invariants]") {
    SessionState state("s");
    state.AppendStreamingThinking("old thinking");
    state.AppendStreamingText("old text");
    state.OnError("boom");   // ends the turn, Error is sticky
    const auto before = state.TakeSnapshot();
    REQUIRE(before.state == SessionStateKind::Error);
    REQUIRE(state.FlatStreamingText() == "old text");   // a turn end does not clear them

    state.OnToolStarted(StartTool("t1"));
    const auto after = state.TakeSnapshot();
    CHECK(after.turn_seq == before.turn_seq + 1);
    CHECK(state.FlatStreamingText().empty());
    CHECK(state.FlatThinkingText().empty());
    CHECK(after.state == SessionStateKind::Working);   // the new turn clears Error
    CHECK(after.turn_in_flight);
}

TEST_CASE("a redelivered tool start is ignored, running or finished",
          "[session_state][invariants]") {
    SessionState state("s");
    state.OnToolStarted(StartTool("t1"));
    const auto version = state.ToolsVersion();
    const auto segments = state.TakeSnapshot().live_segments.size();

    state.OnToolStarted(StartTool("t1"));
    CHECK(state.TakeSnapshot().tools.size() == 1);
    CHECK(state.TakeSnapshot().live_segments.size() == segments);
    CHECK(state.ToolsVersion() == version);

    state.OnToolFinished("t1", FinishTool());
    state.OnToolStarted(StartTool("t1"));
    CHECK(state.TakeSnapshot().tools.size() == 1);
    CHECK_FALSE(state.TakeSnapshot().tools.front().started);
}

// --- ToolsVersion ----------------------------------------------------------------

TEST_CASE("ToolsVersion bumps on every panel-visible change, and only those",
          "[session_state][invariants]") {
    SessionState state("s");
    auto version = state.ToolsVersion();
    const auto bumped = [&] {
        const bool moved = state.ToolsVersion() != version;
        version = state.ToolsVersion();
        return moved;
    };

    state.SeedDisplayNameIfUnset("first");
    CHECK(bumped());
    state.SeedDisplayNameIfUnset("second");   // already named: no-op
    CHECK_FALSE(bumped());

    v1::SessionMetadataEvent rename;
    rename.set_name("renamed");
    state.OnSessionMetadata(rename);
    CHECK(bumped());
    state.OnSessionMetadata(rename);   // same name
    CHECK_FALSE(bumped());

    state.AppendStreamingText("text deltas are not panel content");
    CHECK_FALSE(bumped());

    // Background promote, then the launching call's finish: the row bumps
    // (its status changed) but the transcript does not -- a background ack
    // retires nothing.
    state.OnToolStarted(StartTool("bg"));
    CHECK(bumped());
    state.OnBackgroundTask(Background("bg", v1::BackgroundTaskEvent::STARTED));
    CHECK(bumped());
    const auto counters = state.TakeSnapshot();
    state.OnToolFinished("bg", FinishTool());
    CHECK(bumped());
    CHECK(state.TakeSnapshot().transcript_version == counters.transcript_version);
    CHECK(state.TakeSnapshot().fetch_generation == counters.fetch_generation);
    CHECK(state.TakeSnapshot().tools.front().started);   // still running: only its terminal event ends it

    // Turn end: the live background row survives, nothing swept, no bump.
    state.OnTurnCompleted(v1::TurnCompletedEvent{});
    CHECK_FALSE(bumped());
    CHECK(state.TakeSnapshot().tools.size() == 1);

    // A reopen of a swept row.
    state.OnBackgroundTask(Background("gone", v1::BackgroundTaskEvent::COMPLETED));
    CHECK(bumped());

    // The resync sweep drops running FOREGROUND rows only.
    state.OnToolStarted(StartTool("fg"));
    CHECK(bumped());
    state.OnResyncRequired("gap");
    CHECK(bumped());
    const auto tools = state.TakeSnapshot().tools;
    CHECK(std::none_of(tools.begin(), tools.end(),
                       [](const ToolExecutionState& t) { return t.tool_use_id == "fg"; }));
    state.OnResyncRequired("again");   // nothing left to sweep
    CHECK_FALSE(bumped());
}

// --- live-segment retirement ------------------------------------------------------

TEST_CASE("retirement that leaves a tool marker last closes the open block",
          "[session_state][invariants]") {
    SessionState state("s");
    state.AppendStreamingText("first");
    state.FinalizeStreamingText();
    state.OnToolStarted(StartTool("t1"));
    state.OnToolFinished("t1", FinishTool());
    state.AppendStreamingText("second");   // open block, after the marker
    REQUIRE_FALSE(state.TakeSnapshot().is_streaming_finalized);

    // The page covers "second" (and not t1): the survivors end on t1's marker.
    state.ApplyScrollbackPage(Page({{1, v1::ROLE_ASSISTANT, "second"}}));
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 2);
    CHECK(snap.live_segments.back().kind == LiveTurnSegment::Kind::Tool);
    CHECK(snap.is_streaming_finalized);

    // So the next delta opens a fresh segment instead of fusing onto a
    // retired one.
    state.AppendStreamingText("third");
    snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 3);
    CHECK(*snap.live_segments.back().text_view == "third");
}

TEST_CASE("a persisted thinking block retires its live segment", "[session_state][invariants]") {
    SessionState state("s");
    state.AppendStreamingThinking("pondering");
    state.AppendStreamingText("answer");
    state.ApplyScrollbackPage(Page({{1, v1::ROLE_ASSISTANT, "", "pondering"}}));
    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 1);
    CHECK(snap.live_segments.front().kind == LiveTurnSegment::Kind::Text);
    CHECK(state.FlatThinkingText().empty());
    CHECK(state.FlatStreamingText() == "answer");
}

TEST_CASE("the turn-aware clear keeps a newer turn's thinking", "[session_state][invariants]") {
    SessionState state("s");
    state.AppendStreamingText("turn one");
    state.OnTurnCompleted(v1::TurnCompletedEvent{});
    const auto covered_turn = state.CurrentTurnSeq();
    state.AppendStreamingThinking("turn two thinking");

    state.ClearStreamingIfCurrentTurn(covered_turn);
    CHECK(state.FlatThinkingText() == "turn two thinking");
    CHECK(state.FlatStreamingText().empty());
    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 1);
    CHECK(snap.live_segments.front().kind == LiveTurnSegment::Kind::Thinking);
}

// --- scrollback window ------------------------------------------------------------

TEST_CASE("a delta apply that evicts the oldest rows re-enables load-older",
          "[session_state][invariants]") {
    SessionState state("s");
    state.SetBounds(3, SessionState::kDefaultToolOutputBufferBytes);
    state.ApplyScrollbackPage(Page({{3}, {2}, {1}}, /*has_more=*/false));
    REQUIRE_FALSE(state.CanLoadMore());
    CHECK(state.NewestScrollbackSeq() == 3);

    state.ApplyScrollbackPage(Page({{4}, {5}}, /*has_more=*/false), /*delta=*/true);
    CHECK(state.TakeSnapshot().scrollback_page->size() == 3);
    CHECK(state.NewestScrollbackSeq() == 5);
    CHECK(state.CanLoadMore());
}

TEST_CASE("a delta apply that catches up clears the trimmed marker, a hole sets the gap",
          "[session_state][invariants]") {
    SessionState state("s");
    state.SetBounds(3, SessionState::kDefaultToolOutputBufferBytes);
    state.ApplyScrollbackPage(Page({{6}, {5}, {4}}, /*has_more=*/true));
    state.ApplyScrollbackPage(Page({{3}, {2}}, /*has_more=*/true));   // older: evicts 6, 5
    REQUIRE(state.TakeSnapshot().scrollback_trimmed);
    REQUIRE(state.NewestScrollbackSeq() == 4);

    state.ApplyScrollbackPage(Page({{7}}, /*has_more=*/false), /*delta=*/true);   // 5, 6 missing
    const auto snap = state.TakeSnapshot();
    CHECK_FALSE(snap.scrollback_trimmed);
    CHECK(snap.scrollback_gap);
}

TEST_CASE("a rewind resets the gap and bumps the fetch, leaving prompts, tools and state",
          "[session_state][invariants]") {
    SessionState state("s");
    state.ApplyScrollbackPage(Page({{5}, {4}}));
    state.ApplyScrollbackPage(Page({{2}}));   // 3 missing: a gap
    REQUIRE(state.TakeSnapshot().scrollback_gap);
    state.OnToolStarted(StartTool("t1"));
    state.SetPermissionRequest(Permission("r1"));
    const auto before = state.TakeSnapshot();
    REQUIRE(before.state == SessionStateKind::Waiting);

    state.OnSessionRewound(/*truncated_to_seq=*/5, /*active_last_seq=*/4);
    const auto after = state.TakeSnapshot();
    CHECK_FALSE(after.scrollback_gap);
    CHECK(after.fetch_generation == before.fetch_generation + 1);
    CHECK(after.rewind_epoch == before.rewind_epoch + 1);
    CHECK(after.pending_permission.has_value());
    CHECK(after.tools.size() == 1);
    CHECK(after.state == SessionStateKind::Waiting);
    CHECK(state.NewestScrollbackSeq() == 4);
}

TEST_CASE("a resync bumps both counters and keeps the pending prompt",
          "[session_state][invariants]") {
    SessionState state("s");
    state.SetPermissionRequest(Permission("r1"));
    const auto before = state.TakeSnapshot();
    state.OnResyncRequired("hole");
    const auto after = state.TakeSnapshot();
    CHECK(after.transcript_version == before.transcript_version + 1);
    CHECK(after.fetch_generation == before.fetch_generation + 1);
    CHECK(after.pending_permission.has_value());
}

TEST_CASE("a compaction bumps both counters", "[session_state][invariants]") {
    SessionState state("s");
    const auto before = state.TakeSnapshot();
    v1::CompactionEvent evt;
    evt.set_at_seq(3);
    evt.set_tokens_after(1000);
    state.OnCompaction(evt);
    const auto after = state.TakeSnapshot();
    CHECK(after.transcript_version == before.transcript_version + 1);
    CHECK(after.fetch_generation == before.fetch_generation + 1);
    CHECK(after.input_tokens == 1000);
    REQUIRE(after.compactions->size() == 1);
}

TEST_CASE("refresh failures surface once per streak; a success ends the streak",
          "[session_state][invariants]") {
    SessionState state("s");
    const auto failures_until_notice = SessionState::kScrollbackFailureNoticeAfter;
    for (int i = 1; i < failures_until_notice; ++i) state.NoteRefreshFailed("x");
    CHECK_FALSE(state.TakeSnapshot().latest_notice.has_value());
    state.NoteRefreshFailed("x");
    REQUIRE(state.TakeSnapshot().latest_notice.has_value());
    CHECK(state.TakeSnapshot().latest_notice->is_error);
    const auto notice_seq = state.TakeSnapshot().notice_seq;
    state.NoteRefreshFailed("x");   // past the threshold: no repeat
    CHECK(state.TakeSnapshot().notice_seq == notice_seq);
    CHECK_FALSE(state.TakeSnapshot().refresh_succeeded);

    state.ApplyScrollbackPage(Page({}));   // a success, even empty
    CHECK(state.TakeSnapshot().refresh_succeeded);
    state.ClearNotice();
    for (int i = 1; i < failures_until_notice; ++i) state.NoteRefreshFailed("y");
    CHECK_FALSE(state.TakeSnapshot().latest_notice.has_value());
    state.NoteRefreshFailed("y");
    CHECK(state.TakeSnapshot().latest_notice.has_value());
}

TEST_CASE("the load-older latch admits one fetch at a time", "[session_state][invariants]") {
    SessionState state("s");
    CHECK(state.NewestScrollbackSeq() == -1);
    CHECK(state.TryBeginOlderFetch());
    CHECK_FALSE(state.TryBeginOlderFetch());
    state.EndOlderFetch();
    CHECK(state.TryBeginOlderFetch());
}

// --- pending prompts ----------------------------------------------------------------

TEST_CASE("a text delta clears a pending prompt; a thinking delta does not",
          "[session_state][invariants]") {
    SessionState thinking("s");
    thinking.SetPermissionRequest(Permission("r1"));
    thinking.AppendStreamingThinking("hmm");
    CHECK(thinking.TakeSnapshot().pending_permission.has_value());

    SessionState text("s");
    text.SetPermissionRequest(Permission("r1"));
    text.AppendStreamingText("hi");
    CHECK_FALSE(text.TakeSnapshot().pending_permission.has_value());
}
