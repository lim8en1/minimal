#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/command_executor.hpp"

using namespace minimal;
using namespace std::chrono_literals;

TEST_CASE("posted tasks run", "[command_executor]") {
    rpc::CommandExecutor pool(/*threads=*/2, /*max_backlog=*/8);
    std::atomic<int> ran{0};
    // Fewer posts than the backlog: TryPost may legally refuse when workers
    // have not drained yet, and this case is about execution, not the bound
    // (which the next case tests directly).
    for (int i = 0; i < 6; ++i) {
        REQUIRE(pool.TryPost([&ran] { ++ran; }));
    }
    auto drained = testing::RunInBackground([&] {
        while (ran.load() < 6) std::this_thread::sleep_for(1ms);
        return true;
    });
    REQUIRE(drained.WaitFor(5s));
    CHECK(ran.load() == 6);
}

TEST_CASE("TryPost refuses past the backlog instead of queueing unboundedly",
          "[command_executor][invariant]") {
    // The pool exists because detached per-command threads made a flood of
    // read-only commands an unbounded pile. Unbounding the backlog here would
    // restore exactly that, one layer down.
    //
    // `release` is declared BEFORE the pool on purpose: members are destroyed
    // in reverse order, and the pool's destructor joins a worker that may still
    // be inside the blocker's sleep-then-poll loop. The other order -- which
    // this test once had -- destroyed `release` first, and ASan caught the
    // worker reading a dead atomic on its wake-up.
    std::atomic<bool> release{false};
    rpc::CommandExecutor pool(/*threads=*/1, /*max_backlog=*/2);

    // Park the only worker; nothing drains while it is blocked.
    REQUIRE(pool.TryPost([&release] {
        while (!release.load()) std::this_thread::sleep_for(1ms);
    }));
    std::this_thread::sleep_for(20ms);  // let the worker pick the blocker up

    REQUIRE(pool.TryPost([] {}));
    REQUIRE(pool.TryPost([] {}));
    CHECK_FALSE(pool.TryPost([] {}));  // backlog full: refusal, not a third slot
    CHECK_FALSE(pool.TryPost([] {}));

    release = true;
}

TEST_CASE("destruction joins a running task rather than leaking it",
          "[command_executor]") {
    // A detached thread used to outlive the service here, holding shared_ptrs
    // into registries that were being destroyed. If this join hangs or the
    // task's writes race the test's read, the lifetime contract is broken.
    std::atomic<int> ran{0};
    {
        rpc::CommandExecutor pool(/*threads=*/1, /*max_backlog=*/4);
        REQUIRE(pool.TryPost([&ran] {
            std::this_thread::sleep_for(20ms);
            ++ran;
        }));
    }
    CHECK(ran.load() == 1);
}

TEST_CASE("one key cannot hold more than its share of the pool", "[command_executor]") {
    std::atomic<bool> release{false};
    rpc::CommandExecutor pool(/*threads=*/4, /*max_backlog=*/16,
                              rpc::CommandExecutor::Options{.max_per_key = 2, .deadline = 0ms});
    const auto blocker = [&release] {
        while (!release.load()) std::this_thread::sleep_for(1ms);
    };

    CHECK(pool.TryPost(blocker, "session-a"));
    CHECK(pool.TryPost(blocker, "session-a"));
    CHECK_FALSE(pool.TryPost(blocker, "session-a"));  // a's share is used up
    CHECK(pool.TryPost(blocker, "session-b"));        // another session still gets in

    release = true;
}

TEST_CASE("a task past the deadline is answered and its worker replaced",
          "[command_executor]") {
    std::atomic<bool> release{false};
    std::atomic<bool> timed_out{false};
    std::atomic<int> later_ran{0};
    {
        rpc::CommandExecutor pool(
            /*threads=*/1, /*max_backlog=*/8,
            rpc::CommandExecutor::Options{.max_per_key = 2, .deadline = 40ms, .max_abandoned = 2});

        REQUIRE(pool.TryPost(
            [&release] {
                while (!release.load()) std::this_thread::sleep_for(1ms);
            },
            "s", [&timed_out] { timed_out = true; }));

        auto notified = testing::RunInBackground([&] {
            while (!timed_out.load()) std::this_thread::sleep_for(1ms);
            return true;
        });
        REQUIRE(notified.WaitFor(5s));

        // The only worker is stuck, yet new work still runs: it was replaced.
        REQUIRE(pool.TryPost([&later_ran] { ++later_ran; }, "s"));
        auto ran = testing::RunInBackground([&] {
            while (later_ran.load() == 0) std::this_thread::sleep_for(1ms);
            return true;
        });
        CHECK(ran.WaitFor(5s));
        release = true;
    }
    CHECK(later_ran.load() == 1);
}

TEST_CASE("too many stuck threads make the pool refuse new work", "[command_executor]") {
    std::atomic<bool> release{false};
    std::atomic<int> timeouts{0};
    rpc::CommandExecutor pool(
        /*threads=*/1, /*max_backlog=*/8,
        rpc::CommandExecutor::Options{.max_per_key = 8, .deadline = 40ms, .max_abandoned = 1});
    const auto stuck = [&release] {
        while (!release.load()) std::this_thread::sleep_for(1ms);
    };

    REQUIRE(pool.TryPost(stuck, "s", [&timeouts] { ++timeouts; }));
    auto notified = testing::RunInBackground([&] {
        while (timeouts.load() == 0) std::this_thread::sleep_for(1ms);
        return true;
    });
    REQUIRE(notified.WaitFor(5s));

    CHECK_FALSE(pool.TryPost([] {}, "s"));  // one thread stuck == the cap
    release = true;
}
