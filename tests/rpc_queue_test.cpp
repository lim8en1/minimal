// Unit tier for the async unary RPC queue (T2): a bounded job queue with one
// worker thread, standing in for the unary gRPC calls the TUI used to make on
// the loop thread (see app.cpp's frame-assembly comment and RunTui's
// teardown, where the real queue is joined right after the pump thread).
// These tests exercise only the queue's own contract -- FIFO order, bounded
// drop-oldest backlog, and a clean Stop()/~RpcQueue() -- with no gRPC, no
// AppState, no FTXUI.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/rpc_queue.hpp"

using minimal::client::RpcQueue;

namespace {

// A small rendezvous so a test can block a job in flight, deterministically,
// instead of racing a sleep against the worker thread.
class Gate {
public:
    void Wait() {
        std::unique_lock lk(mtx_);
        cv_.wait(lk, [this] { return open_; });
    }
    void Open() {
        {
            std::lock_guard lk(mtx_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool open_ = false;
};

}  // namespace

TEST_CASE("RpcQueue: jobs run in FIFO order") {
    RpcQueue queue;
    std::mutex mtx;
    std::vector<int> order;
    std::atomic<int> remaining{5};

    for (int i = 0; i < 5; ++i) {
        queue.Submit([&mtx, &order, &remaining, i] {
            {
                std::lock_guard lk(mtx);
                order.push_back(i);
            }
            --remaining;
        });
    }
    while (remaining.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::lock_guard lk(mtx);
    REQUIRE(order == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("RpcQueue: a full queue drops the OLDEST pending job, not the newest submit") {
    Gate release;
    RpcQueue queue(/*capacity=*/2);

    // Job 0 occupies the worker so jobs 1..3 pile up in the backlog while
    // capacity is exercised deterministically.
    std::atomic<bool> job0_started{false};
    queue.Submit([&] {
        job0_started = true;
        release.Wait();
    });
    while (!job0_started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::mutex mtx;
    std::vector<int> ran;
    // Backlog capacity is 2: submitting 1, 2, 3 in that order must drop 1
    // (the oldest queued, not yet running) and keep 2, 3.
    queue.Submit([&] {
        std::lock_guard lk(mtx);
        ran.push_back(1);
    });
    queue.Submit([&] {
        std::lock_guard lk(mtx);
        ran.push_back(2);
    });
    queue.Submit([&] {
        std::lock_guard lk(mtx);
        ran.push_back(3);
    });

    release.Open();
    // Drain: Stop() joins the worker after it finishes whatever it already
    // popped, but queued-and-not-yet-run jobs are discarded by Stop() (see
    // the header) -- so wait for the expected count directly instead.
    for (int i = 0; i < 200 && ([&] {
             std::lock_guard lk(mtx);
             return ran.size();
         }()) < 2;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::lock_guard lk(mtx);
    REQUIRE(ran == std::vector<int>{2, 3});
}

TEST_CASE("RpcQueue: Submit after Stop() is a silent no-op") {
    RpcQueue queue;
    queue.Stop();

    std::atomic<bool> ran{false};
    queue.Submit([&] { ran = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE_FALSE(ran.load());
}

TEST_CASE("RpcQueue: destruction joins the worker without hanging") {
    std::atomic<int> completed{0};
    {
        RpcQueue queue;
        for (int i = 0; i < 3; ++i) {
            queue.Submit([&completed] { ++completed; });
        }
        // ~RpcQueue runs here; it must return (the test times out in the
        // harness otherwise) whether or not every job above finished first --
        // Stop() only guarantees the worker itself is joined.
    }
    SUCCEED("RpcQueue destructor returned");
}
