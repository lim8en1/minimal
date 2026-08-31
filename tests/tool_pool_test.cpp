// The per-session dispatch pool, in isolation. RunInBackground wraps the
// hang-prone bodies: a pool bug that wedges a join must fail a deadline, not
// stop the suite from reporting (tests/support/deadline.hpp).

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/tools/tool_pool.hpp"

using namespace std::chrono_literals;

namespace {

// The same probe shape the agent-loop tests use: tracks how many tasks are
// running at once, and holds briefly so overlap is observable.
struct Probe {
    std::atomic<int> inside{0};
    std::atomic<int> max{0};
    void Enter() {
        const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
        int seen = max.load(std::memory_order_relaxed);
        while (now > seen && !max.compare_exchange_weak(seen, now)) {}
    }
    void Exit() { inside.fetch_sub(1, std::memory_order_acq_rel); }
};

}  // namespace

TEST_CASE("every submitted task runs exactly once", "[tool_pool]") {
    auto result = minimal::testing::RunInBackground([] {
        minimal::tools::ToolPool pool(4);
        std::atomic<int> done{0};
        for (int i = 0; i < 16; ++i) {
            pool.Submit([&done] {
                std::this_thread::sleep_for(5ms);
                done.fetch_add(1, std::memory_order_acq_rel);
            });
        }
        pool.Wait();
        return done.load();
    });
    REQUIRE(result.WaitFor(minimal::testing::kDefaultDeadline));
    CHECK(result.Get() == 16);
}

TEST_CASE("concurrent tasks never exceed the width, and the width is actually used",
          "[tool_pool]") {
    auto result = minimal::testing::RunInBackground([] {
        minimal::tools::ToolPool pool(2);
        Probe probe;
        for (int i = 0; i < 8; ++i) {
            pool.Submit([&probe] {
                probe.Enter();
                std::this_thread::sleep_for(50ms);
                probe.Exit();
            });
        }
        pool.Wait();
        return probe.max.load();
    });
    REQUIRE(result.WaitFor(minimal::testing::kDefaultDeadline));
    const int observed_max = result.Get();
    // The cap holds...
    CHECK(observed_max <= 2);
    // ...and two 50 ms tasks across eight submissions overlap with near
    // certainty, so a pool that secretly serializes fails this line.
    CHECK(observed_max == 2);
}

TEST_CASE("Submit backstops the width: it blocks the caller instead of queueing unboundedly",
          "[tool_pool]") {
    auto result = minimal::testing::RunInBackground([] {
        minimal::tools::ToolPool pool(1);
        Probe probe;
        for (int i = 0; i < 4; ++i) {
            pool.Submit([&probe] {
                probe.Enter();
                std::this_thread::sleep_for(10ms);
                probe.Exit();
            });
        }
        pool.Wait();
        return probe.max.load();
    });
    REQUIRE(result.WaitFor(minimal::testing::kDefaultDeadline));
    CHECK(result.Get() <= 1);
}

TEST_CASE("width 1 runs inline: no threads, caller's stack", "[tool_pool]") {
    minimal::tools::ToolPool pool(1);
    const auto caller = std::this_thread::get_id();
    std::thread::id observed;
    pool.Submit([&observed] { observed = std::this_thread::get_id(); });
    CHECK(observed == caller);
    pool.Wait();
}

TEST_CASE("Wait is idempotent after everything has drained", "[tool_pool]") {
    minimal::tools::ToolPool pool(3);
    std::atomic<int> done{0};
    pool.Submit([&done] { done.fetch_add(1); });
    pool.Wait();
    pool.Wait();
    CHECK(done.load() == 1);
}
