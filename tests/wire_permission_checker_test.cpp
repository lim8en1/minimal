#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/proto_convert.hpp"
#include "minimal/rpc/wire_permission_checker.hpp"

using namespace minimal;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

// The three objects a permission round-trip needs, kept together and refcounted
// so a wedged checker thread can be abandoned without dangling. See
// tests/support/deadline.hpp.
struct Harness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WirePermissionChecker checker{"sess-1", broadcaster, pending};

    // Blocking, like the old queue's Pop: the checker publishes on its own
    // thread, and the transport's drain waits for it.
    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
    std::size_t size() const { return broadcaster.queued(subscriber); }
};

// sessions.unattended_prompt_timeout_seconds set to a real, short bound.
struct TimeoutHarness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WirePermissionChecker checker{"sess-1", broadcaster, pending, 20ms};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

tools::PermissionRequest BashRequest(bool complete = true) {
    tools::PermissionRequest request;
    request.session_id = "sess-1";
    request.tool_name = "bash";
    request.summary = "bash";
    request.category = tools::ToolCategory::Execute;
    request.tool_input = {{"command", "make"}};
    request.disclosure.summary = "make";
    request.disclosure.grants = {{"bash:/usr/bin/make", "/usr/bin/make *"}};
    request.disclosure.complete = complete;
    return request;
}

// The reader thread hands decision+granted_keys across undecoded from any
// particular wire message (see pending_requests.hpp's PermissionAnswer) --
// this mirrors that shape directly rather than round-tripping through a
// serialized v1::PermissionResponse nothing here needs.
rpc::PermissionAnswer Answer(tools::PermissionDecision decision,
                             std::vector<std::string> granted = {}) {
    return rpc::PermissionAnswer{decision, std::move(granted)};
}

}  // namespace

TEST_CASE("a permission request round-trips to an answer", "[wire_permission]") {
    auto harness = std::make_shared<Harness>();

    auto answered = RunInBackground(
        [harness] { return harness->checker.check(BashRequest()); });

    // The checker publishes, then blocks. Draining the queue is the transport's
    // job -- here, this thread's.
    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->has_permission_request());
    const auto& request = event->permission_request();
    CHECK(event->session_id() == "sess-1");
    CHECK(request.tool_name() == "bash");
    CHECK(request.summary() == "make");
    CHECK(request.complete());
    REQUIRE(request.grants_size() == 1);
    CHECK(request.grants(0).key() == "bash:/usr/bin/make");
    CHECK_FALSE(request.request_id().empty());

    CHECK_FALSE(answered.WaitFor(50ms));  // established: genuinely blocked

    CHECK(harness->pending.Resolve(
        request.request_id(),
        Answer(tools::PermissionDecision::Allow, {"bash:/usr/bin/make"})));

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    CHECK(response.decision == tools::PermissionDecision::Allow);
    REQUIRE(response.granted_keys.size() == 1);
    CHECK(response.granted_keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("a deny's message and stop survive the wire checker", "[wire_permission]") {
    auto harness = std::make_shared<Harness>();

    auto answered = RunInBackground(
        [harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->has_permission_request());

    rpc::PermissionAnswer answer = Answer(tools::PermissionDecision::Deny);
    answer.deny_message = "not now";
    answer.stop_turn = true;
    CHECK(harness->pending.Resolve(event->permission_request().request_id(), std::move(answer)));

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    CHECK(response.decision == tools::PermissionDecision::Deny);
    CHECK(response.deny_message == "not now");
    CHECK(response.stop_turn);
}

TEST_CASE("an unattended prompt with a configured timeout denies rather than hanging forever",
          "[wire_permission][S5]") {
    // sessions.unattended_prompt_timeout_seconds's fail-closed opt-in: with
    // nobody attached to answer, the default is to wait forever (see the
    // untimed harness above), but an operator who configured a bound gets
    // Deny instead of a worker parked indefinitely.
    auto harness = std::make_shared<TimeoutHarness>();

    auto answered = RunInBackground(
        [harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());

    REQUIRE(answered.Ready());  // the 20ms timeout elapses well inside the 2s deadline
    CHECK(answered.Get().decision == tools::PermissionDecision::Deny);
}

TEST_CASE("an answer arriving before the timeout still wins",
          "[wire_permission][S5]") {
    auto harness = std::make_shared<TimeoutHarness>();

    auto answered = RunInBackground(
        [harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    const auto& request = event->permission_request();

    CHECK(harness->pending.Resolve(
        request.request_id(),
        Answer(tools::PermissionDecision::Allow, {"bash:/usr/bin/make"})));

    REQUIRE(answered.Ready());
    CHECK(answered.Get().decision == tools::PermissionDecision::Allow);
}

TEST_CASE("a denial round-trips as a denial", "[wire_permission]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    const std::string id = event->permission_request().request_id();
    harness->pending.Resolve(id,
                             Answer(tools::PermissionDecision::Deny));

    REQUIRE(answered.Ready());
    CHECK(answered.Get().decision == tools::PermissionDecision::Deny);
}

TEST_CASE("a grant the request never offered is refused", "[wire_permission]") {
    // The end-to-end version of the filter proto_convert enforces: a client must
    // not be able to mint permission for a binary no prompt ever showed a human.
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    const std::string id = event->permission_request().request_id();
    harness->pending.Resolve(
        id, Answer(tools::PermissionDecision::Allow, {"bash:/usr/bin/make", "bash:/bin/rm"}));

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    REQUIRE(response.granted_keys.size() == 1);
    CHECK(response.granted_keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("an incomplete disclosure says so on the wire", "[wire_permission]") {
    auto harness = std::make_shared<Harness>();
    auto answered =
        RunInBackground([harness] { return harness->checker.check(BashRequest(false)); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    CHECK_FALSE(event->permission_request().complete());

    const std::string id = event->permission_request().request_id();
    harness->pending.Resolve(id,
                             Answer(tools::PermissionDecision::Allow));
    REQUIRE(answered.Ready());
}

TEST_CASE("cancellation unblocks a waiting check as a denial", "[wire_permission]") {
    // Termination must not leave a worker parked on a prompt nobody will answer.
    // Deny is decided HERE rather than inside PendingRequests, which reports only
    // "no answer" -- keeping the policy choice out of the synchronisation
    // primitive.
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->checker.check(BashRequest()); });

    REQUIRE(harness->Pop().has_value());
    CHECK_FALSE(answered.WaitFor(50ms));

    harness->pending.Shutdown();

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    CHECK(response.decision == tools::PermissionDecision::Deny);
    CHECK(response.granted_keys.empty());
}

TEST_CASE("a confirmation answer delivered against a permission id denies structurally",
          "[wire_permission]") {
    // The typed variant makes cross-delivery a type mismatch, not a parse
    // outcome the checker had to remember to guard: a ConfirmationAnswer can
    // never be read as a PermissionAnswer, so the wrong-kind answer fails
    // closed before any parsing happens at all.
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->checker.check(BashRequest()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->permission_request().request_id(),
                             rpc::ConfirmationAnswer{"allow"});

    REQUIRE(answered.Ready());
    CHECK(answered.Get().decision == tools::PermissionDecision::Deny);
}

TEST_CASE("each request gets its own correlation id", "[wire_permission]") {
    auto harness = std::make_shared<Harness>();

    auto first = RunInBackground([harness] { return harness->checker.check(BashRequest()); });
    const auto first_event = harness->Pop();
    REQUIRE(first_event.has_value());
    const std::string first_id = first_event->permission_request().request_id();
    harness->pending.Resolve(
        first_id, Answer(tools::PermissionDecision::Allow));
    REQUIRE(first.Ready());

    auto second = RunInBackground([harness] { return harness->checker.check(BashRequest()); });
    const auto second_event = harness->Pop();
    REQUIRE(second_event.has_value());
    const std::string second_id = second_event->permission_request().request_id();
    CHECK(second_id != first_id);
    harness->pending.Resolve(second_id,
                             Answer(tools::PermissionDecision::Allow));
    REQUIRE(second.Ready());
}

TEST_CASE("answering the previous request does not release the current one",
          "[wire_permission]") {
    // Correlation has to be real: a stale answer arriving late must not be taken
    // as consent for whatever is being asked now.
    auto harness = std::make_shared<Harness>();

    auto first = RunInBackground([harness] { return harness->checker.check(BashRequest()); });
    const auto first_event = harness->Pop();
    REQUIRE(first_event.has_value());
    const std::string first_id = first_event->permission_request().request_id();
    harness->pending.Resolve(
        first_id, Answer(tools::PermissionDecision::Allow));
    REQUIRE(first.Ready());

    auto second = RunInBackground([harness] { return harness->checker.check(BashRequest()); });
    const auto second_event = harness->Pop();
    REQUIRE(second_event.has_value());

    // Replay the first answer. Its slot is gone, so it resolves nothing.
    CHECK_FALSE(harness->pending.Resolve(
        first_id, Answer(tools::PermissionDecision::Allow)));
    CHECK_FALSE(second.WaitFor(100ms));

    const std::string second_id = second_event->permission_request().request_id();
    harness->pending.Resolve(second_id,
                             Answer(tools::PermissionDecision::Deny));
    REQUIRE(second.Ready());
    CHECK(second.Get().decision == tools::PermissionDecision::Deny);
}

TEST_CASE("the worker/reader split is load-bearing, not incidental", "[wire_permission]") {
    // THE INVARIANT the whole three-thread topology exists for.
    //
    // check() blocks its caller. If the thread that must deliver the answer is
    // the same thread that is blocked, nothing can ever arrive. Running the
    // agent loop on the gRPC handler thread would produce exactly this: check()
    // blocks the reader, and the PermissionResponse that would unblock it can
    // only be read by the thread it just blocked.
    //
    // Modelled here as one thread trying to do both jobs in sequence: it never
    // reaches its own drain.
    auto harness = std::make_shared<Harness>();

    auto both_jobs = RunInBackground([harness] {
        harness->checker.check(BashRequest());    // blocks here...
        return harness->Pop().has_value();  // ...so this is unreachable
    });

    CHECK_FALSE(both_jobs.WaitFor(300ms));
    // The request was published and is still sitting unread, because the only
    // thread that was going to read it is blocked waiting for the answer that
    // reading it would have produced. That is the deadlock, exactly.
    CHECK(harness->size() == 1);

    // Released through cancellation rather than by draining, so the assertion
    // above is not quietly undone by the test itself -- and so this does not
    // leak a wedged thread. Once check() returns, the thread reaches its own
    // drain and finds the event still there.
    harness->pending.CancelAll();
    REQUIRE(both_jobs.WaitFor(2s));
    CHECK(both_jobs.Get());
}
