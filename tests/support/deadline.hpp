#pragma once

// Running blocking code under a hard deadline, for tests where A HANG IS THE
// FAILURE MODE UNDER TEST.
//
// std::async is unusable for this. The future it returns blocks in its
// destructor until the task completes, so a wedged worker takes the whole suite
// down with it -- and reports nothing about which invariant broke. That is
// exactly backwards for a test whose purpose is to catch a hang.
//
// So the worker runs DETACHED, and everything it touches is refcounted into the
// callable. A thread that never finishes is leaked along with whatever it
// holds. That is the honest trade: leaking a thread in an already-failing run
// costs nothing, and the alternative is a suite that stops reporting.
//
// CALLER OBLIGATION: the callable must own (by value or shared_ptr) every object
// it touches. Capturing a stack object by reference reintroduces the crash this
// design exists to avoid -- the test returns, the stack dies, the wedged thread
// keeps using it.
//
// Lives in tests/support because it is a build target plugin authors link, and
// any plugin with a blocking interface needs this same shape to test it.

#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace minimal::testing {

inline constexpr std::chrono::milliseconds kDefaultDeadline{2000};

// A result computed on a detached thread, pollable more than once so a test can
// assert "still blocked", then act, then assert "now finished".
template <typename T>
class BackgroundResult {
public:
    template <typename F>
    explicit BackgroundResult(F body) {
        auto promise = std::make_shared<std::promise<T>>();
        future_ = promise->get_future();
        std::thread([promise, body = std::move(body)]() mutable {
            promise->set_value(body());
        }).detach();
    }

    BackgroundResult(BackgroundResult&&) noexcept = default;
    BackgroundResult& operator=(BackgroundResult&&) noexcept = default;
    BackgroundResult(const BackgroundResult&) = delete;
    BackgroundResult& operator=(const BackgroundResult&) = delete;

    bool WaitFor(std::chrono::milliseconds timeout) const {
        return future_.wait_for(timeout) == std::future_status::ready;
    }

    bool Ready() const { return WaitFor(kDefaultDeadline); }

    // Precondition: Ready()/WaitFor() returned true. Safe to call because the
    // future is shared -- an abandoned one is simply never read.
    T Get() const { return future_.get(); }

private:
    std::shared_future<T> future_;
};

template <typename F>
auto RunInBackground(F body) {
    return BackgroundResult<decltype(body())>(std::move(body));
}

// Poll `done` until it flips or the budget expires. For asserting something a
// DETACHED thread does: that thread races the test (it can be descheduled for
// seconds under a parallel ctest), so its effect must be waited for, never
// asserted synchronously after some other wait happens to return. One final
// check runs after the loop so a `done` that flipped between the last sleep
// and the deadline check is still observed.
template <typename Done>
bool WaitForDeadline(std::chrono::milliseconds budget, Done&& done,
                     std::chrono::milliseconds slice = std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(slice);
    }
    return done();
}

}  // namespace minimal::testing
