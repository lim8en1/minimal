// Pure-logic tier: no gRPC, no server. Feeds synthetic v1 protos straight into
// SessionState's On* methods and asserts on TakeSnapshot(). Covers Track E2's
// "event demux" and "state-derivation table" test items against the table in
// ARCHITECTURE.md -- Tab states and attention.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/state.hpp"

using namespace minimal::client;
namespace v1 = minimal::v1;

namespace {

v1::ToolExecutionEvent StartTool(const std::string& id, const std::string& name = "bash") {
    v1::ToolExecutionEvent evt;
    evt.set_tool_use_id(id);
    evt.set_tool_name(name);
    evt.set_started(true);
    evt.set_summary("running " + name);
    return evt;
}

v1::ToolExecutionEvent FinishTool(bool is_error = false) {
    v1::ToolExecutionEvent evt;
    evt.set_started(false);
    evt.set_is_error(is_error);
    return evt;
}

}  // namespace

TEST_CASE("a fresh SessionState is Idle", "[session_state]") {
    SessionState state("s-1");
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Idle);
}

TEST_CASE("TextDelta moves Idle -> Thinking", "[session_state]") {
    SessionState state("s-1");
    state.AppendStreamingText("hello");
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Thinking);
    REQUIRE(state.TakeSnapshot().streaming_text == "hello");
}

TEST_CASE("message_stop with nothing pending returns to Idle", "[session_state]") {
    SessionState state("s-1");
    state.AppendStreamingText("hi");
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Thinking);
    state.FinalizeStreamingText();   // fired on content_block_stop / message_stop
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Idle);
}

TEST_CASE("message_stop with an active tool stays Working, not Idle", "[session_state]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Working);
    state.FinalizeStreamingText();   // a tool-using turn's message_stop between rounds
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Working);
}

TEST_CASE("message_stop with a pending permission stays Waiting", "[session_state]") {
    SessionState state("s-1");
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "r-1";
    state.SetPermissionRequest(std::move(pp));
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Waiting);
    state.FinalizeStreamingText();
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Waiting);
}

TEST_CASE("ToolExecutionEvent{started:true} moves to Working", "[session_state]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1", "read"));
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Working);
    REQUIRE(snap.tools.size() == 1);
    REQUIRE(snap.tools[0].tool_use_id == "t-1");
    REQUIRE(snap.tools[0].started);
}

TEST_CASE("permission/confirmation requests set Waiting and are mutually exclusive",
          "[session_state]") {
    SessionState state("s-1");

    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    pp->tool_name = "bash";
    state.SetPermissionRequest(std::move(pp));
    {
        auto snap = state.TakeSnapshot();
        REQUIRE(snap.state == SessionStateKind::Waiting);
        REQUIRE(snap.pending_permission.has_value());
        REQUIRE_FALSE(snap.pending_confirmation.has_value());
    }

    auto cp = std::make_unique<PendingConfirmation>();
    cp->request_id = "conf-1";
    state.SetConfirmationRequest(std::move(cp));
    {
        // A confirmation supersedes an unanswered permission prompt, and vice versa.
        auto snap = state.TakeSnapshot();
        REQUIRE_FALSE(snap.pending_permission.has_value());
        REQUIRE(snap.pending_confirmation.has_value());
    }

    auto pp2 = std::make_unique<PendingPermission>();
    pp2->request_id = "perm-2";
    state.SetPermissionRequest(std::move(pp2));
    {
        auto snap = state.TakeSnapshot();
        REQUIRE(snap.pending_permission.has_value());
        REQUIRE_FALSE(snap.pending_confirmation.has_value());
    }
}

TEST_CASE("a fresh assistant turn supersedes an unanswered prompt", "[session_state]") {
    SessionState state("s-1");
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));
    REQUIRE(state.TakeSnapshot().pending_permission.has_value());

    state.AppendStreamingText("moving on");
    auto snap = state.TakeSnapshot();
    REQUIRE_FALSE(snap.pending_permission.has_value());
    REQUIRE(snap.state == SessionStateKind::Thinking);
}

TEST_CASE("ClearPendingFor only clears the matching request id", "[session_state]") {
    SessionState state("s-1");
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));

    state.ClearPendingFor("some-other-id");
    REQUIRE(state.TakeSnapshot().pending_permission.has_value());

    state.ClearPendingFor("perm-1");
    REQUIRE_FALSE(state.TakeSnapshot().pending_permission.has_value());
}

TEST_CASE("tool tracking: start -> deltas -> finish, keyed by tool_use_id", "[session_state]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    state.OnToolOutputDelta("t-1", "line one\n");
    state.OnToolOutputDelta("t-1", "line two\n");

    // A delta for an unknown id is dropped, not misattributed to the active tool.
    state.OnToolOutputDelta("t-unknown", "should not appear");

    auto before_finish = state.TakeSnapshot();
    REQUIRE(before_finish.tools.size() == 1);
    REQUIRE(before_finish.tools[0].buffered_output == "line one\nline two\n");
    REQUIRE(before_finish.tools[0].started);

    state.OnToolFinished("t-1", FinishTool(/*is_error=*/false));
    auto after_finish = state.TakeSnapshot();
    REQUIRE_FALSE(after_finish.tools[0].started);
    REQUIRE_FALSE(after_finish.tools[0].is_error);

    // A delta arriving after finish for the same id is dropped -- it is no
    // longer the active execution.
    state.OnToolOutputDelta("t-1", "late, must not append");
    REQUIRE(state.TakeSnapshot().tools[0].buffered_output == "line one\nline two\n");
}

TEST_CASE("OnTurnCompleted records occupancy and clears tools, but does not set Done",
          "[session_state]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    state.OnToolFinished("t-1", FinishTool());
    state.FinalizeStreamingText();   // the message_stop that precedes TurnCompletedEvent
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Idle);

    v1::TurnCompletedEvent evt;
    evt.mutable_usage()->set_input_tokens(100);
    evt.mutable_usage()->set_output_tokens(42);
    state.OnTurnCompleted(evt);

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Idle);   // NOT Done -- turn end != session end
    REQUIRE(snap.input_tokens == 100);
    REQUIRE(snap.output_tokens == 42);
    REQUIRE(snap.tools.empty());
}

TEST_CASE("OnError sets Error and clears pending modals; sticky against OnTurnCompleted",
          "[session_state]") {
    SessionState state("s-1");
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));

    state.OnError("provider exploded");
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Error);
    REQUIRE_FALSE(snap.pending_permission.has_value());

    v1::TurnCompletedEvent evt;
    state.OnTurnCompleted(evt);
    // The turn's completion bookkeeping must not silently clear an in-flight error.
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Error);
}

TEST_CASE("OnSessionEnded is the real Done signal, not turn completion", "[session_state]") {
    SessionState state("s-1");
    state.AppendStreamingText("still going");
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));

    state.OnSessionEnded();
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Done);
    REQUIRE_FALSE(snap.pending_permission.has_value());
}

// --- Scrollback paging -------------------------------------------------------

namespace {

v1::StoredMessage MakeMessage(int64_t seq) {
    v1::StoredMessage m;
    m.set_seq(seq);
    return m;
}

v1::GetMessagesResponse PageOf(std::vector<int64_t> seqs_desc, bool has_more) {
    v1::GetMessagesResponse resp;
    for (auto s : seqs_desc) *resp.add_messages() = MakeMessage(s);
    resp.set_has_more(has_more);
    return resp;
}

}  // namespace

TEST_CASE("first scrollback page becomes the window", "[session_state][scrollback]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({20, 19, 18}, /*has_more=*/true));

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page.size() == 3);
    REQUIRE(snap.scrollback_page.front().seq() == 20);
    REQUIRE(snap.scrollback_page.back().seq() == 18);
    REQUIRE(snap.has_earlier_messages);
    REQUIRE(snap.last_loaded_seq == 18);
    REQUIRE(state.CanLoadMore());
}

TEST_CASE("an older page prepends, growing the window", "[session_state][scrollback]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({20, 19, 18}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({17, 16}, /*has_more=*/false));

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page.size() == 5);
    REQUIRE(snap.scrollback_page.front().seq() == 20);
    REQUIRE(snap.scrollback_page.back().seq() == 16);
    REQUIRE(snap.last_loaded_seq == 16);
    REQUIRE_FALSE(snap.has_earlier_messages);
    REQUIRE_FALSE(state.CanLoadMore());
}

TEST_CASE("a page newer than the current window replaces it (bounded memory)",
          "[session_state][scrollback]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({10, 9, 8}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({30, 29}, /*has_more=*/true));   // e.g. a fresh live page

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page.size() == 2);
    REQUIRE(snap.scrollback_page.front().seq() == 30);
}

TEST_CASE("an empty page still propagates has_more", "[session_state][scrollback]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({}, /*has_more=*/true));
    REQUIRE(state.CanLoadMore());

    state.ApplyScrollbackPage(PageOf({}, /*has_more=*/false));
    REQUIRE_FALSE(state.CanLoadMore());
}

// Invariant: the "older vs. newer" branch in ApplyScrollbackPage is what keeps
// paging backward from resetting the window instead of growing it. This is the
// scrollback-cursor invariant named in Track E2's acceptance criteria -- proven
// load-bearing here, not just asserted to pass by construction.
TEST_CASE("INVARIANT: paging backward twice must grow the window, not reset it",
          "[session_state][scrollback][invariant]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({20, 19, 18}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({17, 16}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({15, 14}, /*has_more=*/false));

    auto snap = state.TakeSnapshot();
    // If the cursor logic were broken (e.g. always replacing instead of
    // conditionally prepending), this would be 2, not 7 -- only the last page
    // applied would survive.
    REQUIRE(snap.scrollback_page.size() == 7);
    REQUIRE(snap.scrollback_page.front().seq() == 20);
    REQUIRE(snap.scrollback_page.back().seq() == 14);
}
