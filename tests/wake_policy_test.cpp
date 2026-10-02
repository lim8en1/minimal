#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/wake_policy.hpp"
#include "minimal/agent/work_queue.hpp"

using minimal::agent::QueuedCommand;
using minimal::agent::QueuedRewind;
using minimal::agent::QueuedTurn;
using minimal::agent::WakeInputs;
using minimal::agent::WorkQueue;
using minimal::agent::WorkerShouldProceed;

TEST_CASE("an idle worker with nothing to do keeps waiting", "[session][wake_policy]") {
    CHECK_FALSE(WorkerShouldProceed(WakeInputs{}));
}

TEST_CASE("termination always wakes", "[session][wake_policy]") {
    WakeInputs in;
    in.terminated = true;
    in.paused = true;
    CHECK(WorkerShouldProceed(in));
}

TEST_CASE("queued work runs unless the pause holds every item", "[session][wake_policy]") {
    WakeInputs in;
    in.queue_empty = false;
    in.queue_has_runnable = true;
    CHECK(WorkerShouldProceed(in));
    in.queue_has_runnable = false;  // paused with only turns queued
    in.paused = true;
    CHECK_FALSE(WorkerShouldProceed(in));
    // Held turns must not fall through to a wake path.
    in.injections_have_user_text = true;
    in.continuation_pending = true;
    CHECK_FALSE(WorkerShouldProceed(in));
}

TEST_CASE("a paused idle session wakes for nothing", "[session][wake_policy]") {
    WakeInputs in;
    in.paused = true;
    in.wake_on_injection = true;
    in.injections_pending = true;
    in.injections_have_user_text = true;
    in.unanswered_after_natural_turn = true;
    in.continuation_pending = true;
    CHECK_FALSE(WorkerShouldProceed(in));
}

TEST_CASE("a finished subagent never wakes (no spin on steered text)", "[session][wake_policy]") {
    WakeInputs in;
    in.subagent_finished = true;
    in.injections_have_user_text = true;
    in.unanswered_after_natural_turn = true;
    in.continuation_pending = true;
    CHECK_FALSE(WorkerShouldProceed(in));
}

TEST_CASE("an injection wakes only when the setting is on and the worker is idle",
          "[session][wake_policy]") {
    WakeInputs in;
    in.injections_pending = true;
    CHECK_FALSE(WorkerShouldProceed(in));
    in.wake_on_injection = true;
    CHECK(WorkerShouldProceed(in));
    in.worker_idle = false;
    CHECK_FALSE(WorkerShouldProceed(in));
}

TEST_CASE("steered user text wakes regardless of wake_on_injection", "[session][wake_policy]") {
    WakeInputs in;
    in.injections_have_user_text = true;
    CHECK(WorkerShouldProceed(in));
    in = {};
    in.unanswered_after_natural_turn = true;
    CHECK(WorkerShouldProceed(in));
}

TEST_CASE("an owed continuation wakes the idle unpaused worker", "[session][wake_policy]") {
    WakeInputs in;
    in.continuation_pending = true;
    CHECK(WorkerShouldProceed(in));
}

TEST_CASE("WorkQueue is bounded and refuses without queueing", "[session][work_queue]") {
    WorkQueue queue;
    for (std::size_t i = 0; i < WorkQueue::kMaxItems; ++i) REQUIRE(queue.TryPush(QueuedCommand{}));
    CHECK(queue.full());
    CHECK_FALSE(queue.TryPush(QueuedCommand{}));
    CHECK(queue.size() == WorkQueue::kMaxItems);
}

TEST_CASE("WorkQueue pause hold leaves turns in place and runs non-turn work",
          "[session][work_queue]") {
    WorkQueue queue;
    QueuedTurn first;
    first.turn_id = 1;
    QueuedTurn second;
    second.turn_id = 2;
    queue.TryPush(first);
    queue.TryPush(QueuedCommand{"r1", "model", {}});
    queue.TryPush(second);
    queue.TryPush(QueuedRewind{});

    CHECK(queue.HasRunnable(true));
    auto popped = queue.PopRunnable(true);
    REQUIRE(popped);
    CHECK(std::holds_alternative<QueuedCommand>(*popped));
    popped = queue.PopRunnable(true);
    REQUIRE(popped);
    CHECK(std::holds_alternative<QueuedRewind>(*popped));
    CHECK_FALSE(queue.HasRunnable(true));
    CHECK_FALSE(queue.PopRunnable(true));
    REQUIRE(queue.size() == 2);

    // Unpaused: strict queue order.
    popped = queue.PopRunnable(false);
    REQUIRE(popped);
    CHECK(std::get<QueuedTurn>(*popped).turn_id == 1);
}

TEST_CASE("WorkQueue removes turns by id and in bulk, keeping commands", "[session][work_queue]") {
    WorkQueue queue;
    QueuedTurn a, b;
    a.turn_id = 1;
    b.turn_id = 2;
    queue.TryPush(a);
    queue.TryPush(QueuedCommand{});
    queue.TryPush(b);

    CHECK_FALSE(queue.RemoveTurn(9));
    CHECK(queue.RemoveTurn(1));
    CHECK(queue.RemoveAllTurns() == std::vector<std::uint64_t>{2});
    CHECK(queue.size() == 1);
}
