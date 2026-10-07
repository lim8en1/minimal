// TaskWorker (minimal/client/sdk/task_worker.hpp): the serial queue a
// client plugin runs its blocking calls on.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "minimal/client/sdk/task_worker.hpp"

using minimal::client::plugins::TaskWorker;
using namespace std::chrono_literals;

namespace {

// A gate a task can block on until the test opens it.
class Gate {
public:
    void Wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return open_; });
    }
    void Open() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool open_ = false;
};

template <typename Pred>
bool Eventually(Pred pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("TaskWorker runs tasks in order, one at a time, and calls after_each",
          "[task_worker]") {
    std::mutex mtx;
    std::vector<int> ran;
    std::atomic<int> after{0};
    TaskWorker worker("test", [&] { ++after; });
    for (int i = 0; i < 20; ++i) {
        worker.Post([&, i] {
            std::lock_guard<std::mutex> lock(mtx);
            ran.push_back(i);
        });
    }
    REQUIRE(Eventually([&] { return after.load() == 20; }));
    std::lock_guard<std::mutex> lock(mtx);
    REQUIRE(ran.size() == 20);
    for (int i = 0; i < 20; ++i) CHECK(ran[i] == i);
}

TEST_CASE("TaskWorker contains a throwing task and keeps going", "[task_worker]") {
    std::atomic<bool> second{false};
    std::atomic<int> after{0};
    TaskWorker worker("test", [&] { ++after; });
    worker.Post([] { throw std::runtime_error("boom"); });
    worker.Post([] { throw 42; });
    worker.Post([&] { second = true; });
    REQUIRE(Eventually([&] { return second.load(); }));
    CHECK(Eventually([&] { return after.load() == 3; }));  // after_each runs for a throw too
}

TEST_CASE("TaskWorker's destructor finishes the running task and drops the queued ones",
          "[task_worker]") {
    Gate started;
    Gate release;
    std::atomic<bool> first_done{false};
    std::atomic<bool> queued_ran{false};
    auto worker = std::make_unique<TaskWorker>("test");
    worker->Post([&] {
        started.Open();
        release.Wait();
        first_done = true;
    });
    worker->Post([&] { queued_ran = true; });
    started.Wait();

    std::thread destroyer([&] { worker.reset(); });
    std::this_thread::sleep_for(20ms);  // the destructor is now waiting on the running task
    release.Open();
    destroyer.join();
    CHECK(first_done);
    CHECK_FALSE(queued_ran);
}
