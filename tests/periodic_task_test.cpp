#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "minimal/common/periodic_task.hpp"

using minimal::common::PeriodicTask;
using namespace std::chrono_literals;

TEST_CASE("a throwing tick is logged and the task keeps ticking", "[periodic_task][invariant]") {
    std::atomic<int> ticks{0};
    PeriodicTask task([] { return 1ms; }, [&] {
        ++ticks;
        throw std::runtime_error("boom");
    });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (ticks.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(ticks.load() >= 3);
}

TEST_CASE("a tick may stop its own task", "[periodic_task]") {
    std::atomic<int> ticks{0};
    std::atomic<PeriodicTask*> self{nullptr};
    PeriodicTask task([] { return 1ms; }, [&] {
        PeriodicTask* const me = self.load();
        if (me == nullptr) return;  // ticked before the test published the pointer
        ++ticks;
        me->Stop();  // signals only; the destructor below joins
    });
    self = &task;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (ticks.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);
    CHECK(ticks.load() == 1);
}
