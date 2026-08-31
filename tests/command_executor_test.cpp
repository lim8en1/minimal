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
    rpc::CommandExecutor pool(/*threads=*/1, /*max_backlog=*/2);

    // Park the only worker; nothing drains while it is blocked.
    std::atomic<bool> release{false};
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
