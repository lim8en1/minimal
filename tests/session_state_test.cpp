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

v1::BackgroundTaskEvent BackgroundEvent(const std::string& tool_use_id,
                                        v1::BackgroundTaskEvent::State state,
                                        const std::string& command = "make -j8") {
    v1::BackgroundTaskEvent evt;
    evt.set_tool_use_id(tool_use_id);
    evt.set_state(state);
    evt.set_command(command);
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

// Regression: MaybeGoIdleLocked used to check only tools_.back().started --
// the LAST-STARTED tool, not "any tool still running". ARCHITECTURE.md "Tool
// dispatch is parallel" makes several tools of one round run at once (width
// 4 by default) and finish in COMPLETION order, not gate order, so the
// last-started tool finishing first is the common case, not an edge case.
// The old check flipped the tab to Idle -- spinner gone, "Working" lost --
// with earlier calls of the same round still executing on the pool.
TEST_CASE("message_stop stays Working while ANY parallel tool is still running, "
          "even the last-started one finishing first",
          "[session_state][regression]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    state.OnToolStarted(StartTool("t-2"));
    state.OnToolStarted(StartTool("t-3"));   // last-started -> tools_.back()
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Working);

    // t-3 (the last-started, formerly the only one MaybeGoIdleLocked looked
    // at) finishes FIRST -- t-1 and t-2 are still running on the pool.
    state.OnToolFinished("t-3", FinishTool());
    state.FinalizeStreamingText();   // a tool-using turn's message_stop between rounds
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.state == SessionStateKind::Working);   // NOT Idle: two tools still active

    state.OnToolFinished("t-1", FinishTool());
    state.OnToolFinished("t-2", FinishTool());
    state.FinalizeStreamingText();
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Idle);   // now nothing is active
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

TEST_CASE("an ask sets Waiting, supersedes the other pending modals, and is superseded by them",
          "[session_state]") {
    SessionState state("s-1");

    auto ask = std::make_unique<PendingAsk>();
    ask->request_id = "ask-1";
    state.SetAskRequest(std::move(ask));
    {
        auto snap = state.TakeSnapshot();
        REQUIRE(snap.state == SessionStateKind::Waiting);
        REQUIRE(snap.pending_ask.has_value());
        REQUIRE(snap.pending_ask->request.questions().empty());  // verbatim request storage
        REQUIRE_FALSE(snap.pending_permission.has_value());
        REQUIRE_FALSE(snap.pending_confirmation.has_value());
    }

    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));
    REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());

    auto ask2 = std::make_unique<PendingAsk>();
    ask2->request_id = "ask-2";
    state.SetAskRequest(std::move(ask2));
    REQUIRE_FALSE(state.TakeSnapshot().pending_permission.has_value());
    REQUIRE(state.TakeSnapshot().pending_ask.has_value());

    auto cp = std::make_unique<PendingConfirmation>();
    cp->request_id = "conf-1";
    state.SetConfirmationRequest(std::move(cp));
    REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());
    REQUIRE(state.TakeSnapshot().pending_confirmation.has_value());
}

TEST_CASE("ClearPendingFor matches an ask by request id and MaybeGoIdle resets Waiting",
          "[session_state]") {
    SessionState state("s-1");
    auto ask = std::make_unique<PendingAsk>();
    ask->request_id = "ask-1";
    state.SetAskRequest(std::move(ask));

    state.ClearPendingFor("ask-other");
    REQUIRE(state.TakeSnapshot().pending_ask.has_value());
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Waiting);

    state.ClearPendingFor("ask-1");
    REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());

    state.OnPendingResolved();
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Idle);
}

TEST_CASE("a fresh assistant turn and terminal events clear a pending ask", "[session_state]") {
    SECTION("new turn") {
        SessionState state("s-1");
        auto ask = std::make_unique<PendingAsk>();
        ask->request_id = "ask-1";
        state.SetAskRequest(std::move(ask));
        state.AppendStreamingText("moving on");
        REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());
    }
    SECTION("error") {
        SessionState state("s-1");
        auto ask = std::make_unique<PendingAsk>();
        ask->request_id = "ask-1";
        state.SetAskRequest(std::move(ask));
        state.OnError("dead");
        REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());
    }
    SECTION("session ended") {
        SessionState state("s-1");
        auto ask = std::make_unique<PendingAsk>();
        ask->request_id = "ask-1";
        state.SetAskRequest(std::move(ask));
        state.OnSessionEnded();
        REQUIRE_FALSE(state.TakeSnapshot().pending_ask.has_value());
    }
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
    // TakeSnapshot (the DEEP, focused-tab snapshot) deliberately does NOT
    // carry buffered_output: its only tool consumer (ChatView::RenderLiveToolRow)
    // reads name/started/is_error/summary, never the buffer -- the
    // running-tools panel is what renders that, off TakeLightSnapshot(true)
    // below (track B1: copying up to 64 KiB per tool on every deep snapshot
    // for a consumer that ignores it was pure waste).
    REQUIRE(before_finish.tools[0].buffered_output.empty());
    REQUIRE(before_finish.tools[0].started);

    auto light = state.TakeLightSnapshot(/*include_tool_output=*/true);
    REQUIRE(light.tools.size() == 1);
    REQUIRE(light.tools[0].buffered_output == "line one\nline two\n");

    state.OnToolFinished("t-1", FinishTool(/*is_error=*/false));
    auto after_finish = state.TakeSnapshot();
    REQUIRE_FALSE(after_finish.tools[0].started);
    REQUIRE_FALSE(after_finish.tools[0].is_error);

    // A delta arriving after finish for the same id is dropped -- it is no
    // longer the active execution.
    state.OnToolOutputDelta("t-1", "late, must not append");
    REQUIRE(state.TakeLightSnapshot(/*include_tool_output=*/true).tools[0].buffered_output ==
           "line one\nline two\n");
}

// --- background bash rows (ARCHITECTURE.md "Background bash rows") --------

TEST_CASE("a background task promotes its ToolExecutionEvent row rather than duplicating it",
          "[session_state][background]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));           // the gating ack
    state.OnBackgroundTask(
        BackgroundEvent("t-1", v1::BackgroundTaskEvent::STARTED, "make -j8"));
    // The synchronous call ack: the launching tool call returns quickly even
    // though the detached process keeps running.
    state.OnToolFinished("t-1", FinishTool());

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.tools.size() == 1);   // promoted, not a second row
    CHECK(snap.tools[0].is_background);
    CHECK(snap.tools[0].started);      // the background task is still live
    CHECK(snap.tools[0].summary == "make -j8");
}

TEST_CASE("OnTurnCompleted keeps a LIVE background row but sweeps a finished one",
          "[session_state][background]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    state.OnBackgroundTask(BackgroundEvent("t-1", v1::BackgroundTaskEvent::STARTED));
    state.OnToolFinished("t-1", FinishTool());   // ack; the task itself keeps running

    // The turn that launched it ends while the background task is still
    // running -- its row must survive the panel reset, unlike a foreground
    // tool row.
    state.OnToolStarted(StartTool("t-2"));       // an ordinary foreground tool, same turn
    state.OnToolFinished("t-2", FinishTool());
    state.OnTurnCompleted(v1::TurnCompletedEvent{});

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.tools.size() == 1);
    CHECK(snap.tools[0].tool_use_id == "t-1");   // t-2 (foreground) was swept
    CHECK(snap.tools[0].is_background);
    CHECK(snap.tools[0].started);

    // The background task finishes, possibly turns later. Its terminal event
    // closes the row that has been sitting live since before.
    state.OnBackgroundTask(BackgroundEvent("t-1", v1::BackgroundTaskEvent::COMPLETED));
    auto after_terminal = state.TakeSnapshot();
    REQUIRE(after_terminal.tools.size() == 1);
    CHECK_FALSE(after_terminal.tools[0].started);
    CHECK_FALSE(after_terminal.tools[0].is_error);

    // The NEXT turn boundary sweeps the now-finished background row, same as
    // any other finished row.
    state.OnTurnCompleted(v1::TurnCompletedEvent{});
    CHECK(state.TakeSnapshot().tools.empty());
}

TEST_CASE("a background task's terminal event REOPENS a row the panel already swept",
          "[session_state][background]") {
    // The doc's own words: "a client keyed by tool_use_id alone must
    // tolerate a row reopening after its finish event." Simulates the STARTED
    // notice never having reached this client (or the row already having
    // been sitting quiescent past a turn boundary) -- the terminal event is
    // the first this client hears of the task, and must still surface it.
    SessionState state("s-1");
    state.OnBackgroundTask(
        BackgroundEvent("t-9", v1::BackgroundTaskEvent::FAILED, "flaky-build.sh"));

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.tools.size() == 1);
    CHECK(snap.tools[0].is_background);
    CHECK_FALSE(snap.tools[0].started);
    CHECK(snap.tools[0].is_error);
    CHECK(snap.tools[0].summary == "flaky-build.sh");
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
    REQUIRE(snap.scrollback_page->size() == 3);
    REQUIRE(snap.scrollback_page->front().seq() == 20);
    REQUIRE(snap.scrollback_page->back().seq() == 18);
    REQUIRE(snap.has_earlier_messages);
    REQUIRE(snap.last_loaded_seq == 18);
    REQUIRE(state.CanLoadMore());
}

// THE RENDER-FREEZE REGRESSION: the persisted user row only reached a page
// through a fetch, and nothing fetched mid-turn, so a submitted message did
// not render until the turn ENDED -- after the assistant's streaming reply.
// OnChatSubmitted echoes the turn optimistically; a page apply past the
// submit-time baseline retires it in favor of the persisted (expanded) row.
TEST_CASE("a submitted turn echoes optimistically until the persisted row lands",
          "[session_state][optimistic]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({10, 9}, /*has_more=*/false));

    state.OnChatSubmitted("hi @test.tar.gz");
    auto snap = state.TakeSnapshot();
    CHECK(snap.pending_user_text == "hi @test.tar.gz");
    CHECK(snap.state == SessionStateKind::Working);   // accepted onto the wire

    SECTION("a page apply with no newer seq keeps the echo") {
        // The server has not persisted anything yet (a raced fetch).
        state.ApplyScrollbackPage(PageOf({10, 9}, /*has_more=*/false));
        CHECK(state.TakeSnapshot().pending_user_text == "hi @test.tar.gz");
    }

    SECTION("a page apply past the baseline retires the echo") {
        // The user row (seq 11) -- and anything after it -- is now in the page.
        state.ApplyScrollbackPage(PageOf({11, 10, 9}, /*has_more=*/false));
        CHECK(state.TakeSnapshot().pending_user_text.empty());
        CHECK(state.TakeSnapshot().scrollback_page->front().seq() == 11);
    }

    SECTION("an error clears the echo: nothing will retire it otherwise") {
        state.OnError("connection lost");
        CHECK(state.TakeSnapshot().pending_user_text.empty());
    }

    SECTION("a session end clears the echo") {
        state.OnSessionEnded();
        CHECK(state.TakeSnapshot().pending_user_text.empty());
    }
}

TEST_CASE("snapshots between applies share one page buffer", "[session_state][perf]") {
    // The per-event deep copy: TakeSnapshot used to copy every stored message
    // on EVERY wire event (one call per maintenance pass), which during a
    // busy turn meant a full-window copy per stream delta. The page is now
    // copy-on-apply: successive snapshots between applies are the SAME
    // buffer, and only an apply that changes rows produces a new one.
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({5, 4, 3}, /*has_more=*/false));
    auto first = state.TakeSnapshot();
    auto second = state.TakeSnapshot();
    REQUIRE(first.scrollback_page != nullptr);
    CHECK(first.scrollback_page.get() == second.scrollback_page.get());

    state.ApplyScrollbackPage(PageOf({6, 5, 4, 3}, /*has_more=*/false));
    auto third = state.TakeSnapshot();
    CHECK(third.scrollback_page.get() != first.scrollback_page.get());
    CHECK(third.scrollback_page->front().seq() == 6);
    // The old snapshot's view is untouched by the new apply.
    CHECK(first.scrollback_page->front().seq() == 5);
}

TEST_CASE("an older page prepends, growing the window", "[session_state][scrollback]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({20, 19, 18}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({17, 16}, /*has_more=*/false));

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page->size() == 5);
    REQUIRE(snap.scrollback_page->front().seq() == 20);
    REQUIRE(snap.scrollback_page->back().seq() == 16);
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
    REQUIRE(snap.scrollback_page->size() == 2);
    REQUIRE(snap.scrollback_page->front().seq() == 30);
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
    REQUIRE(snap.scrollback_page->size() == 7);
    REQUIRE(snap.scrollback_page->front().seq() == 20);
    REQUIRE(snap.scrollback_page->back().seq() == 14);
}

// Track R1b-5: the notice used to be built by a hand-copied subset of the
// mapper, silently dropping display/view_hint -- every View-display result
// (e.g. /sessions) arrived as a STATUS line and its view never opened. The
// sequence number is what lets the frame's maintain pass perform a View
// notice's side-effects exactly once; without it the guard cannot exist.
TEST_CASE("command notice maps display/view_hint and advances the notice sequence",
          "[session_state][notice]") {
    SessionState state("s-1");

    v1::CommandResult evt;
    evt.set_message("6 sessions");
    evt.set_status(v1::COMMAND_STATUS_OK);
    evt.set_display(v1::COMMAND_DISPLAY_VIEW);
    evt.set_view_hint("sessions");
    state.OnCommandResult(evt);

    auto snap = state.TakeSnapshot();
    REQUIRE(snap.latest_notice.has_value());
    CHECK(snap.latest_notice->message == "6 sessions");
    CHECK(snap.latest_notice->display == v1::COMMAND_DISPLAY_VIEW);
    CHECK(snap.latest_notice->view_hint == "sessions");
    CHECK(snap.notice_seq == 1);

    // A second result is a NEW notice: the seq must move, or a maintain pass
    // that already handled the first one would never handle the replacement.
    v1::CommandResult second;
    second.set_message("not a session");
    second.set_status(v1::COMMAND_STATUS_ERROR);
    second.set_display(v1::COMMAND_DISPLAY_STATUS);
    state.OnCommandResult(second);

    snap = state.TakeSnapshot();
    REQUIRE(snap.latest_notice.has_value());
    CHECK(snap.latest_notice->is_error);
    CHECK(snap.latest_notice->view_hint.empty());
    CHECK(snap.notice_seq == 2);
}

// Regression: a stale /-command notice (an old /help listing, a stale error)
// used to sit pinned across the WHOLE next turn -- persisted transcript rows
// are the record of what happened, so a status line about the LAST command
// has nothing to say about a turn the user just started.
TEST_CASE("submitting a chat turn clears a stale command notice", "[session_state][notice]") {
    SessionState state("s-1");

    v1::CommandResult evt;
    evt.set_message("11 commands");
    evt.set_status(v1::COMMAND_STATUS_OK);
    evt.set_display(v1::COMMAND_DISPLAY_STATUS);
    state.OnCommandResult(evt);
    REQUIRE(state.TakeSnapshot().latest_notice.has_value());
    const uint64_t seq_before = state.TakeSnapshot().notice_seq;

    state.OnChatSubmitted("what next");

    const auto snap = state.TakeSnapshot();
    CHECK_FALSE(snap.latest_notice.has_value());
    CHECK(snap.notice_seq > seq_before);
}

// Track R2-4: the scrollback window is a bounded SLIDING window. Loading older
// pages beyond the cap evicts from the window's NEWEST end and flags it -- the
// pre-R2 append grew client memory without bound (a long PageUp on a 1M-token
// history materialized the whole transcript). The flag exists so the view can
// mark the boundary instead of showing a silent gap; a newest-slice refresh
// resets both.
TEST_CASE("INVARIANT: paging older beyond the cap evicts newest pages and flags the trim",
          "[session_state][scrollback][invariant]") {
    SessionState state("s-1");
    state.SetBounds(/*scrollback_max_messages=*/5, /*tool_output_buffer_bytes=*/64 * 1024);

    state.ApplyScrollbackPage(PageOf({99, 98, 97, 96, 95}, /*has_more=*/true));
    REQUIRE(state.TakeSnapshot().scrollback_page->size() == 5);
    REQUIRE_FALSE(state.TakeSnapshot().scrollback_trimmed);

    // Each older page appends 3 rows; the window must stay at 5, evicting from
    // the newest end.
    state.ApplyScrollbackPage(PageOf({94, 93, 92}, /*has_more=*/true));
    auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page->size() == 5);
    REQUIRE(snap.scrollback_trimmed);
    CHECK(snap.scrollback_page->front().seq() == 96);   // 99, 98, 97 evicted (8 rows -> 5)
    CHECK(snap.scrollback_page->back().seq() == 92);

    // A newest-slice refresh resets the trim flag and the window.
    state.ApplyScrollbackPage(PageOf({99, 98, 97}, /*has_more=*/false));
    snap = state.TakeSnapshot();
    CHECK_FALSE(snap.scrollback_trimmed);
    CHECK(snap.scrollback_page->size() == 3);
}

// THE DUPLICATE-FETCH REGRESSION: two "load older" presses inside one RPC
// round trip fetch the same before_seq page. The second apply used to fall
// through to the newer/assign branch (its newest seq is no longer below the
// window's oldest) and REPLACE the window with the older page -- the newest
// rows vanished from the view.
TEST_CASE("a fully-overlapping older page is a no-op, not a window replacement",
          "[session_state][scrollback][regression]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({20, 19, 18}, /*has_more=*/true));
    state.ApplyScrollbackPage(PageOf({17, 16}, /*has_more=*/false));

    // The raced duplicate: the SAME before_seq page applied a second time.
    state.ApplyScrollbackPage(PageOf({17, 16}, /*has_more=*/false));

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page->size() == 5);   // not reset to 2
    REQUIRE(snap.scrollback_page->front().seq() == 20);   // the newest rows survive
    // Strictly descending: nothing was duplicated either.
    for (std::size_t i = 1; i < snap.scrollback_page->size(); ++i) {
        CHECK((*snap.scrollback_page)[i - 1].seq() > (*snap.scrollback_page)[i].seq());
    }
    CHECK(snap.last_loaded_seq == 16);
}

// SessionRewoundEvent: the server cut the transcript tail (rewind) or restored
// it (redo). The client trims its window to the ACTIVE transcript (rows with
// seq <= active_last_seq) and clears any streaming/pending-turn state -- the
// cut rows are gone for good. The version bump inside is what makes the
// refresh state machine refetch what survived.
TEST_CASE("OnSessionRewound trims the window to the active transcript and clears live state",
          "[session_state][rewind]") {
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({40, 39, 38, 37}, /*has_more=*/false));
    state.AppendStreamingText("partial reply of the discarded turn");
    state.OnChatSubmitted("the turn being rewound away");
    state.RequestCancel();
    const uint64_t version_before = state.TakeSnapshot().transcript_version;

    state.OnSessionRewound(/*truncated_to_seq=*/39, /*active_last_seq=*/38);

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page->size() == 2);   // rows past active_last_seq are gone
    CHECK(snap.scrollback_page->front().seq() == 38);
    CHECK(snap.scrollback_page->back().seq() == 37);
    CHECK(snap.last_loaded_seq == 37);
    // Rows survived: conservatively older pages may exist (a spurious empty
    // "load older" round trip costs nothing; a missing one loses history).
    CHECK(snap.has_earlier_messages);
    // Live/pending-turn state goes with the discarded turn.
    CHECK(snap.streaming_text.empty());
    CHECK(snap.live_segments.empty());
    CHECK(snap.pending_user_text.empty());
    CHECK_FALSE(snap.cancel_requested);
    // The refetch triggers: version mismatch for the refresh state machine,
    // refresh_seq bump for any fetch in flight across the rewind.
    CHECK(snap.transcript_version > version_before);
    CHECK(snap.refresh_succeeded);
}

TEST_CASE("OnSessionRewound REDO clears the whole window for a refetch",
          "[session_state][rewind]") {
    // Restored rows were never in the client's window; truncated_to_seq == -1
    // marks a redo -- only a clean clear plus a refetch is correct.
    SessionState state("s-1");
    state.ApplyScrollbackPage(PageOf({40, 39, 38}, /*has_more=*/false));
    state.AppendStreamingText("live text");

    state.OnSessionRewound(/*truncated_to_seq=*/-1, /*active_last_seq=*/42);

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.scrollback_page->empty());
    CHECK(snap.last_loaded_seq == -1);
    CHECK(snap.has_earlier_messages);
    CHECK(snap.streaming_text.empty());
}

// Track R2-5: the tool-output "ring buffer" caps AT APPEND (head 1/4 + marker +
// tail 3/4), mirroring the server's persisted-result cap. The pre-R2 code
// appended without bound and truncated only at RENDER time, so a chatty tool
// grew client memory for the whole turn.
TEST_CASE("INVARIANT: tool output deltas stay bounded, with marker and tail preserved",
          "[session_state][tools][invariant]") {
    SessionState state("s-1");
    state.SetBounds(2000, /*tool_output_buffer_bytes=*/4096);

    state.OnToolStarted(StartTool("t1"));
    const std::string chunk(1024, 'x');
    for (int i = 0; i < 16; ++i) {
        state.OnToolOutputDelta("t1", chunk);   // 16 KB total vs a 4 KB cap
    }
    // The running-tools panel is the only renderer of buffered_output, and it
    // reads the LIGHT snapshot (include_tool_output=true) -- see the "tool
    // tracking" test case above for why TakeSnapshot() no longer carries it.
    auto snap = state.TakeLightSnapshot(/*include_tool_output=*/true);
    REQUIRE(snap.tools.size() == 1);
    const auto& out = snap.tools[0].buffered_output;
    CHECK(out.size() <= 4096 + 256);   // bounded (small slack for the marker)
    CHECK(out.find("live output trimmed") != std::string::npos);   // marker present
    CHECK(out.find("xxx") != std::string::npos);   // head AND tail content preserved
}

// {tokens} liveness: usage events arrive at message start (input half) and
// message_delta (output half) DURING a turn; before OnUsage existed the
// segment sat empty until TurnCompleted.
TEST_CASE("OnUsage updates the token counters mid-turn", "[session_state]") {
    SessionState state("s-1");

    v1::UsageEvent evt;
    evt.mutable_usage()->set_input_tokens(1200);
    evt.mutable_usage()->set_output_tokens(34);
    evt.mutable_usage()->set_cache_read_input_tokens(100);
    evt.mutable_usage()->set_cache_creation_input_tokens(200);
    state.OnUsage(evt);

    const auto snap = state.TakeSnapshot();
    CHECK(snap.input_tokens == 1200);
    CHECK(snap.output_tokens == 34);
    CHECK(snap.cache_read_tokens == 100);
    CHECK(snap.cache_write_tokens == 200);

    // The turn-end tally is still authoritative: it overwrites.
    v1::TurnCompletedEvent done;
    done.mutable_usage()->set_input_tokens(1300);
    done.mutable_usage()->set_output_tokens(50);
    state.OnTurnCompleted(done);
    const auto final_snap = state.TakeSnapshot();
    CHECK(final_snap.input_tokens == 1300);
    CHECK(final_snap.output_tokens == 50);
}

// Track R2-8: stream recreation keys on turn_seq, not a buffer-size heuristic.
// The seq must advance once per streaming turn (a new stream after the previous
// one ended), and must NOT advance on intra-turn boundaries.
TEST_CASE("turn_seq advances once per streaming turn", "[session_state]") {    SessionState state("s-1");

    state.AppendStreamingText("hel");
    state.AppendStreamingText("lo");
    CHECK(state.TakeSnapshot().turn_seq == 1);

    state.FinalizeStreamingText();   // message boundary WITHIN a turn: no bump
    state.AppendStreamingText("more");
    CHECK(state.TakeSnapshot().turn_seq == 1);

    state.OnTurnCompleted(v1::TurnCompletedEvent{});   // ends the turn
    state.AppendStreamingText("next turn");
    CHECK(state.TakeSnapshot().turn_seq == 2);
}

// A1: a new content block WITHIN the same turn (text -> tool -> text, or
// thinking -> text) must not fuse its raw markdown onto the previous block's
// -- an unclosed fence from block one would otherwise eat block two whole.
// The first delta of the turn gets no leading separator (nothing precedes
// it); a turn boundary (OnTurnCompleted) gets none either, since streaming_text
// is cleared at the turn boundary in practice (SessionState::ClearStreaming,
// called by the post-turn scrollback apply) before the next turn's first delta.
TEST_CASE("AppendStreamingText separates blocks within a turn, not across turns",
          "[session_state]") {
    SessionState state("s-1");

    state.AppendStreamingText("block one");
    CHECK(state.TakeSnapshot().streaming_text == "block one");

    state.FinalizeStreamingText();   // content_block_stop: block one ends
    state.AppendStreamingText("block two");   // same turn, a later block
    CHECK(state.TakeSnapshot().streaming_text == "block one\n\nblock two");

    // A THIRD block does not double up the separator against the second's.
    state.FinalizeStreamingText();
    state.AppendStreamingText("block three");
    CHECK(state.TakeSnapshot().streaming_text == "block one\n\nblock two\n\nblock three");
}

// C4: live_segments records this turn's text and tool activity in ARRIVAL
// order, not two separate groups (all text, then all tools). This is what
// lets the live chat view interleave a tool call between the text before
// and after it, matching the shape the PERSISTED rows decompose into
// (ChatRowsFromMessages) instead of only converging to that shape once the
// turn ends.
TEST_CASE("live_segments records text and tool activity in arrival order",
          "[session_state][live_segments]") {
    using Kind = LiveTurnSegment::Kind;
    SessionState state("s-1");

    state.AppendStreamingText("intro text");
    state.FinalizeStreamingText();   // content_block_stop: the text block ends
    state.OnToolStarted(StartTool("t-1", "bash"));
    state.OnToolFinished("t-1", FinishTool(/*is_error=*/false));
    state.AppendStreamingText("closing text");   // a NEW block, after the tool

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 3);
    CHECK(snap.live_segments[0].kind == Kind::Text);
    CHECK(snap.live_segments[0].text == "intro text");
    CHECK(snap.live_segments[1].kind == Kind::Tool);
    CHECK(snap.live_segments[1].tool_use_id == "t-1");
    CHECK(snap.live_segments[2].kind == Kind::Text);
    CHECK(snap.live_segments[2].text == "closing text");   // its OWN text, not the whole turn

    // streaming_text (the flat convenience field) still carries everything,
    // separator-joined, for consumers that only need "is anything
    // streaming" or the total text as one string.
    CHECK(snap.streaming_text == "intro text\n\nclosing text");
}

// PARALLEL tool dispatch (ARCHITECTURE.md "Tool dispatch is parallel", width
// 4 by default): several tools started together each get their OWN marker,
// in GATE order -- the same order snapshot.tools already preserves.
TEST_CASE("live_segments gives each parallel tool its own marker, in gate order",
          "[session_state][live_segments]") {
    using Kind = LiveTurnSegment::Kind;
    SessionState state("s-1");

    state.AppendStreamingText("running a few tools");
    state.FinalizeStreamingText();
    state.OnToolStarted(StartTool("t-1", "bash"));
    state.OnToolStarted(StartTool("t-2", "read"));
    state.OnToolStarted(StartTool("t-3", "grep"));

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 4);
    CHECK(snap.live_segments[0].kind == Kind::Text);
    CHECK(snap.live_segments[1].tool_use_id == "t-1");
    CHECK(snap.live_segments[2].tool_use_id == "t-2");
    CHECK(snap.live_segments[3].tool_use_id == "t-3");
}

// live_segments follows streaming_text's exact clear lifecycle: cleared by
// ClearStreaming (the post-turn scrollback apply, once persisted rows take
// over) and by OnResyncRequired (a hole in event delivery -- drop the live
// half, the transcript is authoritative), never by the turn merely ending.
TEST_CASE("live_segments clears alongside streaming_text, not before",
          "[session_state][live_segments]") {
    SessionState state("s-1");
    state.AppendStreamingText("hello");
    state.OnToolStarted(StartTool("t-1"));
    REQUIRE(state.TakeSnapshot().live_segments.size() == 2);

    v1::TurnCompletedEvent done;
    state.OnTurnCompleted(done);
    // The turn ending alone does NOT clear it -- the live content stays
    // visible until the refresh actually swaps in the persisted rows,
    // exactly like streaming_text.
    CHECK(state.TakeSnapshot().live_segments.size() == 2);

    state.ClearStreaming();
    CHECK(state.TakeSnapshot().live_segments.empty());

    state.AppendStreamingText("next turn");
    state.OnResyncRequired("test");
    CHECK(state.TakeSnapshot().live_segments.empty());
    CHECK(state.TakeSnapshot().streaming_text.empty());
}

// Thinking deltas are live segments too (the OpenAI-compatible backends
// dropped them outright before provider capture existed; the TUI then never
// rendered the accumulation). Reasoning typically OPENS a turn, so its
// segment must carry the same turn_seq the following text segments do.
TEST_CASE("thinking deltas become Thinking live segments in arrival order",
          "[session_state][live_segments]") {
    using Kind = LiveTurnSegment::Kind;
    SessionState state("s-1");

    state.AppendStreamingThinking("pondering");
    state.AppendStreamingThinking(" more");
    // Text resumes: a NEW segment, after the thinking one.
    state.AppendStreamingText("the answer");
    state.FinalizeStreamingText();
    // A second reasoning stretch: its own segment, after the text's.
    state.AppendStreamingThinking("second thought");

    const auto snap = state.TakeSnapshot();
    REQUIRE(snap.live_segments.size() == 3);
    CHECK(snap.live_segments[0].kind == Kind::Thinking);
    CHECK(snap.live_segments[0].text == "pondering more");
    CHECK(snap.live_segments[1].kind == Kind::Text);
    CHECK(snap.live_segments[1].text == "the answer");
    CHECK(snap.live_segments[2].kind == Kind::Thinking);
    CHECK(snap.live_segments[2].text == "second thought");
    // Same turn stamp as everything else in the turn.
    CHECK(snap.live_segments[0].turn_seq == snap.turn_seq);

    // The flat convenience accumulation still carries everything.
    CHECK(snap.streaming_thinking_text == "pondering moresecond thought");
}

TEST_CASE("thinking segments follow the shared live_segments lifecycle",
          "[session_state][live_segments]") {
    SessionState state("s-1");

    state.AppendStreamingThinking("turn one reasoning");
    REQUIRE(state.TakeSnapshot().live_segments.size() == 1);

    v1::TurnCompletedEvent done;
    state.OnTurnCompleted(done);
    // The turn ending alone does NOT clear it -- same as text segments.
    CHECK(state.TakeSnapshot().live_segments.size() == 1);

    // A new turn opens with thinking: turn_seq bumps, the new segment stamps
    // with the NEW turn. The turn-1 segment lingers (the turn ending alone
    // does not clear anything) until the turn-aware clear retires BOTH.
    state.AppendStreamingThinking("turn two reasoning");
    CHECK(state.TakeSnapshot().turn_seq == 2);
    REQUIRE(state.TakeSnapshot().live_segments.size() == 2);
    CHECK(state.TakeSnapshot().live_segments[0].turn_seq == 1);
    CHECK(state.TakeSnapshot().live_segments[1].turn_seq == 2);
    CHECK(state.TakeSnapshot().live_segments[1].text == "turn two reasoning");

    // The turn-aware clear retires them like any other segment.
    state.ClearStreamingIfCurrentTurn(2);
    CHECK(state.TakeSnapshot().live_segments.empty());
    CHECK(state.TakeSnapshot().streaming_thinking_text.empty());
}

// THE STALE-CLEAR REGRESSION: the newest refresh applied its page then cleared
// streaming UNCONDITIONALLY -- if the user's next turn had already started
// streaming (a submit raced the fetch), its opening deltas were wiped until
// the turn ended: a visible transcript gap. The clear is turn-aware now: the
// persisted page covers the turn live at SUBMIT time, not whatever turn is
// current at apply time.
TEST_CASE("the newest-page clear keeps a NEWER turn's live text", "[session_state][streaming]") {
    SessionState state("s-1");

    SECTION("the common case -- turn ended, no new turn: the clear still happens") {
        state.AppendStreamingText("turn one");
        state.FinalizeStreamingText();
        state.OnTurnCompleted(v1::TurnCompletedEvent{});
        REQUIRE(state.TakeSnapshot().turn_seq == 1);

        // A page captured at turn 1 applies; the live text is exactly what the
        // persisted rows replace. Keeping it here would double-render.
        state.ClearStreamingIfCurrentTurn(1);
        CHECK(state.TakeSnapshot().streaming_text.empty());
        CHECK(state.TakeSnapshot().live_segments.empty());
    }

    SECTION("a newer turn started since -- its live text survives") {
        state.AppendStreamingText("turn one");
        state.FinalizeStreamingText();
        state.OnTurnCompleted(v1::TurnCompletedEvent{});
        REQUIRE(state.TakeSnapshot().turn_seq == 1);

        // Turn 2 begins before the fetch captured at turn 1 applies.
        state.AppendStreamingText("turn two opening deltas");
        REQUIRE(state.TakeSnapshot().turn_seq == 2);

        state.ClearStreamingIfCurrentTurn(1);   // the stale, turn-1 capture
        CHECK(state.TakeSnapshot().streaming_text == "turn two opening deltas");
        REQUIRE(state.TakeSnapshot().live_segments.size() == 1);
        CHECK(state.TakeSnapshot().live_segments[0].text == "turn two opening deltas");

        // A fetch captured at the CURRENT turn clears as before.
        state.ClearStreamingIfCurrentTurn(2);
        CHECK(state.TakeSnapshot().streaming_text.empty());
        CHECK(state.TakeSnapshot().live_segments.empty());
    }
}

// ---------------------------------------------------------------------------
// Interrupt (the TUI's cancel verb) and the confirmation-answer state fix.
// ---------------------------------------------------------------------------

TEST_CASE("RequestCancel flags a busy tab; the turn end clears it", "[session_state]") {
    SessionState state("s-1");

    // Nothing in flight: a cancel press is a no-op, not a stuck flag.
    state.RequestCancel();
    CHECK_FALSE(state.TakeSnapshot().cancel_requested);

    state.AppendStreamingText("working");
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Thinking);
    state.RequestCancel();
    REQUIRE(state.TakeSnapshot().cancel_requested);

    // The turn actually ending -- cancelled or not -- is what retires the flag.
    // (The message stop precedes turn completion and resolves Thinking -> Idle.)
    state.FinalizeStreamingText();
    state.OnTurnCompleted(v1::TurnCompletedEvent{});
    CHECK_FALSE(state.TakeSnapshot().cancel_requested);

    // Waiting counts as busy too: the server's cancel answers a parked prompt
    // with a denial, so a wedged prompt is interruptible.
    auto pp = std::make_unique<PendingPermission>();
    pp->request_id = "perm-1";
    state.SetPermissionRequest(std::move(pp));
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Waiting);
    state.RequestCancel();
    CHECK(state.TakeSnapshot().cancel_requested);
    state.OnError("connection lost");
    CHECK_FALSE(state.TakeSnapshot().cancel_requested);
}

TEST_CASE("answering a command confirmation leaves Waiting -- the wedge fix",
          "[session_state]") {
    // A command confirmation (/clear) is answered with no
    // tool events or message stops following: the server worker was BLOCKED
    // on the answer, so nothing else re-derives the tab state. Without
    // OnPendingResolved the tab stayed stranded in Waiting with the '!' glyph
    // and nothing pending -- the exact wedge the confirmation-modal wiring
    // exists to kill.
    SessionState state("s-1");

    auto cp = std::make_unique<PendingConfirmation>();
    cp->request_id = "confirm-1";
    cp->options = {"Yes", "No"};
    state.SetConfirmationRequest(std::move(cp));
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Waiting);

    // AnswerConfirmation = ClearPendingFor + OnPendingResolved (state.cpp).
    state.ClearPendingFor("confirm-1");
    state.OnPendingResolved();
    auto snap = state.TakeSnapshot();
    CHECK(snap.state == SessionStateKind::Idle);
    CHECK_FALSE(snap.pending_confirmation.has_value());
}

TEST_CASE("OnPendingResolved does not clobber an active tool", "[session_state]") {
    SessionState state("s-1");
    state.OnToolStarted(StartTool("t-1"));
    REQUIRE(state.TakeSnapshot().state == SessionStateKind::Working);

    state.OnPendingResolved();   // nothing pending: the guard keeps Working
    CHECK(state.TakeSnapshot().state == SessionStateKind::Working);
}

// ---------------------------------------------------------------------------
// Desired agent/model state (the pre/post-session unification): the client
// owns what the user CHOSE; the wire metadata reports what the session RUNS.
// ConvergenceCommands is the turn-start diff SubmitChatTurn submits.
// ---------------------------------------------------------------------------

TEST_CASE("convergence commands: /agent before /model, guarded on unknown effective state",
          "[session_state][desired]") {
    SessionState state("s-1");
    CHECK(state.ConvergenceCommands().empty());   // nothing desired, nothing effective

    state.SetDesiredAgentPreset("alt");
    state.SetDesiredModel("main/other-model");

    // No effective metadata yet: the create may still be in flight carrying
    // the desired state, so a blind switch would be pure noise (and on a
    // sessionless tab there is nothing to converge into at all).
    CHECK(state.ConvergenceCommands().empty());
    auto snap = state.TakeSnapshot();
    CHECK(snap.desired_agent_preset == "alt");
    CHECK(snap.desired_model == "main/other-model");
    CHECK(snap.agent_preset.empty());   // effective untouched by staging

    v1::SessionMetadataEvent meta;
    meta.set_agent_preset("main");
    meta.set_model("main/test-model");
    state.OnSessionMetadata(meta);

    // THE ORDER IS THE PRECEDENCE RULE: the server has a user-chosen model
    // survive an /agent switch, so the agent command must land FIRST.
    const auto cmds = state.ConvergenceCommands();
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0] == "/agent alt");
    CHECK(cmds[1] == "/model main/other-model");
}

TEST_CASE("convergence commands: a desired value equal to effective is a no-op",
          "[session_state][desired]") {
    SessionState state("s-1");
    state.SetDesiredModel("main/test-model");

    v1::SessionMetadataEvent meta;
    meta.set_agent_preset("main");
    meta.set_model("main/test-model");
    state.OnSessionMetadata(meta);

    CHECK(state.ConvergenceCommands().empty());   // already converged
}

TEST_CASE("convergence commands: the confirming metadata event silences the diff",
          "[session_state][desired]") {
    SessionState state("s-1");
    state.SetDesiredAgentPreset("alt");

    v1::SessionMetadataEvent before;
    before.set_agent_preset("main");
    before.set_model("main/test-model");
    state.OnSessionMetadata(before);
    REQUIRE(state.ConvergenceCommands().size() == 1);

    // The switch ran server-side; the metadata event now reports the desired
    // value as effective -- converged, nothing more to send.
    v1::SessionMetadataEvent after;
    after.set_agent_preset("alt");
    after.set_model("main/test-model");
    state.OnSessionMetadata(after);
    CHECK(state.ConvergenceCommands().empty());

    const auto snap = state.TakeSnapshot();
    CHECK(snap.agent_preset == "alt");
    CHECK(snap.desired_agent_preset == "alt");   // sticky: the user's choice stands
}
