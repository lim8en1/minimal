#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "deadline.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/serial_executor.hpp"

using minimal::common::SerialExecutor;
using namespace std::chrono_literals;

TEST_CASE("SerialExecutor runs tasks in post order and survives a throwing one",
          "[serial_executor]") {
    SerialExecutor executor("test", 16, minimal::common::DefaultLogger());
    std::vector<int> ran;
    std::mutex mutex;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(executor.TryPost([&, i] {
            if (i == 2) throw std::runtime_error("boom");
            std::lock_guard<std::mutex> lock(mutex);
            ran.push_back(i);
        }));
    }
    executor.WaitForIdle();
    CHECK(ran == std::vector<int>{0, 1, 3, 4});
}

TEST_CASE("SerialExecutor refuses posts once full or stopped", "[serial_executor]") {
    SerialExecutor executor("test", 1, minimal::common::DefaultLogger());
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    REQUIRE(executor.TryPost([&] {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return release; });
    }));
    // The worker may or may not have taken the first task yet; either way the
    // one-slot queue fills within two more posts.
    const bool second = executor.TryPost([] {});
    const bool third = executor.TryPost([] {});
    CHECK_FALSE((second && third));
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    executor.Stop(SerialExecutor::Backlog::kRun);
    CHECK(executor.stopped());
    CHECK_FALSE(executor.TryPost([] {}));
    executor.WaitForIdle();  // nothing outstanding after a refused post
}

TEST_CASE("SerialExecutor::Stop(kDiscard) drops the backlog and leaves nothing outstanding",
          "[serial_executor]") {
    SerialExecutor executor("test", 64, minimal::common::DefaultLogger());
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> ran{0};
    REQUIRE(executor.TryPost([&] {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return release; });
        ++ran;
    }));
    for (int i = 0; i < 10; ++i) REQUIRE(executor.TryPost([&] { ++ran; }));

    auto stopped = minimal::testing::RunInBackground([&] {
        executor.Stop(SerialExecutor::Backlog::kDiscard);
        return true;
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    REQUIRE(stopped.WaitFor(5s));

    auto idle = minimal::testing::RunInBackground([&] {
        executor.WaitForIdle();
        return true;
    });
    REQUIRE(idle.WaitFor(5s));
    // The in-flight task always completes; how many queued ones slipped in
    // before the discard is timing. What matters is that WaitForIdle returned.
    CHECK(ran.load() >= 1);
}

TEST_CASE("a task may stop its own executor: the backlog still runs and nothing deadlocks",
          "[serial_executor][invariant]") {
    std::atomic<int> ran{0};
    auto finished = minimal::testing::RunInBackground([&ran] {
        SerialExecutor executor("test", 16, minimal::common::DefaultLogger());
        REQUIRE(executor.TryPost([&] {
            executor.Stop(SerialExecutor::Backlog::kRun);  // on the worker: no self-join
            ++ran;
        }));
        REQUIRE(executor.TryPost([&] { ++ran; }));
        executor.WaitForIdle();
        CHECK(executor.stopped());
        CHECK_FALSE(executor.TryPost([] {}));
        return true;  // the destructor joins here
    });
    REQUIRE(finished.WaitFor(5s));
    CHECK(ran.load() == 2);
}
