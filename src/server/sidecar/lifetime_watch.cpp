#include "minimal/sidecar/lifetime_watch.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace minimal::sidecar {

namespace {
struct GuardState {
    std::atomic<bool> fired{false};
};
}  // namespace

std::shared_ptr<void> WatchLifetime(std::weak_ptr<const void> lifetime,
                                    std::function<void()> on_expired,
                                    std::chrono::milliseconds slice) {
    auto guard = std::make_shared<GuardState>();
    std::weak_ptr<GuardState> weak_guard = guard;

    // Detached, by the same reasoning as every background watcher: the
    // session's death is not synchronizable with this thread, and the thread
    // touches only the lifetime-safe weak_ptrs it owns. One-shot: it fires
    // `on_expired` at most once, then ends.
    //
    // Cancellation is guard death, not a flag: the thread re-locks the guard
    // each slice, so when the caller releases it the thread observes that and
    // exits without ever firing. (A flag would not do -- the thread cannot
    // hold a strong reference to the thing it is told to stop wanting.)
    std::thread([weak_guard = std::move(weak_guard),
                 lifetime = std::move(lifetime),
                 on_expired = std::move(on_expired), slice] {
        while (true) {
            const auto locked = weak_guard.lock();
            if (!locked) return;  // guard released: cleanup disarmed
            if (lifetime.expired()) {
                // The lifetime expired first: fire exactly once. The guard
                // dying after this point cannot retract a fired cleanup --
                // the caller's `on_expired` must be idempotent against its
                // own explicit teardown, which it races anyway (the same
                // accepted asymmetry as killpg landing near an explicit
                // kill).
                bool expected = false;
                if (!locked->fired.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel)) {
                    return;
                }
                on_expired();
                return;
            }
            std::this_thread::sleep_for(slice);
        }
    }).detach();

    // The returned guard is the disarm token: destroying it (explicit
    // teardown) makes the thread's next lock fail. Erasing the type keeps the
    // caller's member a plain `std::shared_ptr<void>`.
    return std::shared_ptr<void>(guard, static_cast<void*>(nullptr));
}

}  // namespace minimal::sidecar
