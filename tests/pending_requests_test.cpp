#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/pending_requests.hpp"

using minimal::rpc::ConfirmationAnswer;
using minimal::rpc::PendingAnswer;
using minimal::rpc::PendingRequests;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

using PendingPtr = std::shared_ptr<PendingRequests>;
using RegistrationPtr = std::shared_ptr<PendingRequests::Registration>;

RegistrationPtr Register(const PendingPtr& pending, std::string request_id) {
    return std::make_shared<PendingRequests::Registration>(
        pending->Register(std::move(request_id)));
}

// Waiters capture BOTH shared_ptrs by value, never the enclosing stack: if the
// invariant under test is broken the thread wedges, is abandoned, and must not
// be left pointing at a destroyed PendingRequests. See tests/support/deadline.hpp.
auto AwaitInBackground(PendingPtr pending, RegistrationPtr registration) {
    return RunInBackground([pending, registration] { return registration->AwaitResponse(); });
}

}  // namespace

TEST_CASE("an answer arriving after the wait begins unblocks it", "[pending_requests]") {
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    auto waiter = AwaitInBackground(pending, registration);
    CHECK_FALSE(waiter.WaitFor(50ms));  // established: it is actually blocked

    CHECK(pending->Resolve("req-1", ConfirmationAnswer{"allow"}));

    REQUIRE(waiter.Ready());
    const auto answer = waiter.Get();
    REQUIRE(answer.has_value());
    CHECK(std::get<ConfirmationAnswer>(*answer).chosen_option == "allow");
}

TEST_CASE("an answer arriving before the wait begins is not lost", "[pending_requests]") {
    // The ordering race, and the reason registration is separate from waiting.
    // The worker registers, publishes the request, and only then blocks -- so a
    // fast client's answer routinely lands in the window between publish and
    // wait. If that answer were dropped, the worker would park forever on a
    // question the human already answered.
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    CHECK(pending->Resolve("req-1", ConfirmationAnswer{"deny"}));

    auto waiter = AwaitInBackground(pending, registration);
    REQUIRE(waiter.Ready());
    const auto answer = waiter.Get();
    REQUIRE(answer.has_value());
    CHECK(std::get<ConfirmationAnswer>(*answer).chosen_option == "deny");
}

TEST_CASE("resolving an unregistered id is a harmless no-op", "[pending_requests]") {
    // A client may answer a request whose session already terminated. Dropping
    // it is correct; buffering it for a hypothetical future waiter would let a
    // client grow this map without bound by answering things nobody asked.
    PendingRequests pending;
    CHECK_FALSE(pending.Resolve("never-registered", ConfirmationAnswer{"allow"}));
    CHECK(pending.pending_count() == 0);
}

TEST_CASE("concurrent waiters on different ids do not steal each other's answers",
          "[pending_requests]") {
    auto pending = std::make_shared<PendingRequests>();
    auto first = Register(pending, "req-1");
    auto second = Register(pending, "req-2");

    auto waiter_one = AwaitInBackground(pending, first);
    auto waiter_two = AwaitInBackground(pending, second);

    pending->Resolve("req-2", ConfirmationAnswer{"second-answer"});
    pending->Resolve("req-1", ConfirmationAnswer{"first-answer"});

    REQUIRE(waiter_one.Ready());
    REQUIRE(waiter_two.Ready());
    CHECK(std::get<ConfirmationAnswer>(waiter_one.Get().value()).chosen_option == "first-answer");
    CHECK(std::get<ConfirmationAnswer>(waiter_two.Get().value()).chosen_option == "second-answer");
}

TEST_CASE("resolving one id leaves another waiter blocked", "[pending_requests]") {
    // The mirror of the case above: notify_all wakes every waiter, so each must
    // re-check that the answer is actually ITS answer before returning.
    auto pending = std::make_shared<PendingRequests>();
    auto first = Register(pending, "req-1");
    auto second = Register(pending, "req-2");

    auto waiter_two = AwaitInBackground(pending, second);
    pending->Resolve("req-1", ConfirmationAnswer{"not-for-you"});

    CHECK_FALSE(waiter_two.WaitFor(200ms));

    pending->Resolve("req-2", ConfirmationAnswer{"mine"});
    REQUIRE(waiter_two.Ready());
    CHECK(std::get<ConfirmationAnswer>(waiter_two.Get().value()).chosen_option == "mine");
}

TEST_CASE("cancellation wakes a blocked waiter", "[pending_requests]") {
    // Termination reuses the cancellation path; if this hangs, terminating a
    // session with a permission prompt outstanding would never join its worker.
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    auto waiter = AwaitInBackground(pending, registration);
    CHECK_FALSE(waiter.WaitFor(50ms));

    pending->CancelAll();

    REQUIRE(waiter.Ready());
    CHECK_FALSE(waiter.Get().has_value());
}

TEST_CASE("shutdown is permanent, so a late waiter does not park", "[pending_requests]") {
    // A worker can reach a permission gate while teardown is already running.
    // If shutdown only woke the waiters that existed at that moment, that
    // worker would block on a reader thread which has already stopped.
    auto pending = std::make_shared<PendingRequests>();
    pending->Shutdown();

    auto registration = Register(pending, "req-1");
    auto waiter = AwaitInBackground(pending, registration);
    REQUIRE(waiter.Ready());
    CHECK_FALSE(waiter.Get().has_value());
}

TEST_CASE("cancellation is NOT permanent: the next waiter still blocks and is answerable",
          "[pending_requests]") {
    // The counterpart to the shutdown case above, and the reason the two are
    // separate methods. Session::Cancel ends a TURN and leaves the session able
    // to accept the next one; if it shared the permanent latch, every permission
    // prompt after the first CancelTurn would return "no answer" instantly and
    // WirePermissionChecker would turn that into Deny -- silently, for the life
    // of the session.
    auto pending = std::make_shared<PendingRequests>();

    auto cancelled = Register(pending, "req-1");
    auto abandoned = AwaitInBackground(pending, cancelled);
    CHECK_FALSE(abandoned.WaitFor(50ms));
    pending->CancelAll();
    REQUIRE(abandoned.Ready());
    CHECK_FALSE(abandoned.Get().has_value());

    // The next turn's prompt: must block for a real answer, not return nullopt.
    auto next = Register(pending, "req-2");
    auto waiter = AwaitInBackground(pending, next);
    CHECK_FALSE(waiter.WaitFor(50ms));

    CHECK(pending->Resolve("req-2", ConfirmationAnswer{"allow"}));
    REQUIRE(waiter.Ready());
    CHECK(std::get<ConfirmationAnswer>(waiter.Get().value()).chosen_option == "allow");
}

TEST_CASE("an answer racing its own cancellation is not delivered", "[pending_requests]") {
    // Resolve and CancelAll can both land before the waiter runs. The turn is
    // over either way, so the answer belongs to nobody: returning it would let a
    // tool run for a turn the user already cancelled.
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    CHECK(pending->Resolve("req-1", ConfirmationAnswer{"allow"}));
    pending->CancelAll();

    auto waiter = AwaitInBackground(pending, registration);
    REQUIRE(waiter.Ready());
    CHECK_FALSE(waiter.Get().has_value());
}

TEST_CASE("cancellation wakes every outstanding waiter, not just one", "[pending_requests]") {
    auto pending = std::make_shared<PendingRequests>();
    auto first = Register(pending, "req-1");
    auto second = Register(pending, "req-2");

    auto waiter_one = AwaitInBackground(pending, first);
    auto waiter_two = AwaitInBackground(pending, second);
    CHECK_FALSE(waiter_one.WaitFor(50ms));

    pending->CancelAll();

    REQUIRE(waiter_one.Ready());
    REQUIRE(waiter_two.Ready());
    CHECK_FALSE(waiter_one.Get().has_value());
    CHECK_FALSE(waiter_two.Get().has_value());
}

TEST_CASE("a registration releases its slot when it goes out of scope", "[pending_requests]") {
    PendingRequests pending;
    {
        auto registration = pending.Register("req-1");
        CHECK(pending.pending_count() == 1);
    }
    CHECK(pending.pending_count() == 0);
    CHECK_FALSE(pending.Resolve("req-1", ConfirmationAnswer{"allow"}));
}

TEST_CASE("a slot is released even when the waiter unwinds", "[pending_requests]") {
    // The waiter is the session worker running tool dispatch, which can throw.
    // Without RAII an exception would leave a permanent entry keyed by an id
    // nobody will ever resolve again.
    PendingRequests pending;
    try {
        auto registration = pending.Register("req-1");
        throw std::runtime_error("tool blew up mid-dispatch");
    } catch (const std::runtime_error&) {
    }
    CHECK(pending.pending_count() == 0);
}

TEST_CASE("the first answer wins over a retransmission", "[pending_requests]") {
    // A duplicate is a client retransmitting, not changing its mind. Overwriting
    // an answer the worker may already have acted on would be worse than
    // ignoring it.
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    CHECK(pending->Resolve("req-1", ConfirmationAnswer{"allow"}));
    CHECK(pending->Resolve("req-1", ConfirmationAnswer{"deny"}));

    auto waiter = AwaitInBackground(pending, registration);
    REQUIRE(waiter.Ready());
    CHECK(std::get<ConfirmationAnswer>(waiter.Get().value()).chosen_option == "allow");
}

TEST_CASE("many concurrent registrations each get their own answer", "[pending_requests]") {
    constexpr int kCount = 64;
    auto pending = std::make_shared<PendingRequests>();

    std::vector<RegistrationPtr> registrations;
    std::vector<minimal::testing::BackgroundResult<std::optional<PendingAnswer>>> waiters;
    registrations.reserve(kCount);
    waiters.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        registrations.push_back(Register(pending, "req-" + std::to_string(i)));
        waiters.push_back(AwaitInBackground(pending, registrations.back()));
    }

    // Resolve in reverse, so answer order never matches wait order.
    for (int i = kCount - 1; i >= 0; --i) {
        pending->Resolve("req-" + std::to_string(i),
                         ConfirmationAnswer{"answer-" + std::to_string(i)});
    }

    for (int i = 0; i < kCount; ++i) {
        INFO("waiter " << i);
        REQUIRE(waiters[i].Ready());
        CHECK(std::get<ConfirmationAnswer>(waiters[i].Get().value()).chosen_option ==
              "answer-" + std::to_string(i));
    }
}

TEST_CASE("a cross-delivered answer keeps its type", "[pending_requests]") {
    // The typed payload: the variant travels with the answer, so a waiter that
    // registered under a confirmation id can never misread a serialized
    // PermissionResponse as a chosen option -- the mismatch is structural
    // (std::get returns null / throws), not a parse failure the waiter had to
    // remember to guard.
    auto pending = std::make_shared<PendingRequests>();
    auto registration = Register(pending, "req-1");

    CHECK(pending->Resolve("req-1", minimal::rpc::PermissionAnswer{"<serialized proto>"}));

    auto waiter = AwaitInBackground(pending, registration);
    REQUIRE(waiter.Ready());
    const auto answer = waiter.Get();
    REQUIRE(answer.has_value());
    CHECK(std::holds_alternative<minimal::rpc::PermissionAnswer>(*answer));
}
