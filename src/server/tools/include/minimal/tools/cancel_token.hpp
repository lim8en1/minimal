#pragma once

// Event-driven cancellation for waits that would otherwise poll.
//
// `is_cancelled` (the polling `std::function<bool()>` in ToolContext) stays:
// tools that check cancellation occasionally are served fine by polling. What
// polling cannot do is wake a BLOCKED waiter promptly -- the MCP connection
// layer parks in a condition_variable wait, and a 25 ms poll there meant 40
// wakeups per second per in-flight call, forever, for nothing.
//
// The design is deliberately minimal: the token owns one mutex + cv, and BOTH
// events a waiter cares about -- its slot completing and cancellation firing
// -- funnel through it. A waiter checks its slot under the token's mutex and
// waits on the token's cv; completion notifies the same cv. No waiter
// registration lists, no per-waiter state: CancelAll() notifies every live
// token it minted, and Notify() from the completion path wakes whoever is
// parked. Lock order is one-directional (worker: slot.mu -> token.mu; waiter:
// token.mu, released before it touches slot.mu), so no cycle exists.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace minimal::tools {

class CancelToken {
public:
    bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }

    // Fires the token and wakes every waiter parked on it. release, not
    // acq_rel: acq_rel is invalid on a store (libc++ diagnose_if warns), and
    // the acquire half is meaningless here -- waiters acquire through Mu()
    // below.
    void Cancel() {
        cancelled_.store(true, std::memory_order_release);
        Notify();
    }

    // Wakes waiters WITHOUT cancelling -- the completion path. The empty
    // critical section closes the lost-wakeup window: a waiter either saw the
    // state it re-checks after waking, or was not yet parked and cannot miss
    // the notify because it must acquire mu_ (below) to park.
    void Notify() {
        { std::lock_guard<std::mutex> lock(mu_); }
        cv_.notify_all();
    }

    // Waiter-side access. The waiter holds Mu() across its predicate checks
    // and its wait, so Notify/Cancel are serialized against them.
    std::mutex& Mu() { return mu_; }
    std::condition_variable& Cv() { return cv_; }

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mu_;
    std::condition_variable cv_;
};

using CancelTokenPtr = std::shared_ptr<CancelToken>;

// Mints one token per turn and fires them all on cancel. Weak registry: a
// token nobody holds anymore costs nothing to carry and is pruned as Mint()
// goes. Session owns one source; the current turn's token rides in
// ToolContext through ToolExecutor into every tool.
class CancelSource {
public:
    CancelTokenPtr Mint() {
        auto token = std::make_shared<CancelToken>();
        std::lock_guard<std::mutex> lock(mu_);
        tokens_.push_back(token);
        tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                                     [](const std::weak_ptr<CancelToken>& t) { return t.expired(); }),
                      tokens_.end());
        return token;
    }

    // Cancels every token minted and still alive. Exactly the semantics
    // CancelTurn wants: the running turn's tools wake, and any straggler from
    // an already-dead turn is a no-op (nobody waits on it).
    //
    // Cancellation happens UNDER mu_, not on a snapshot taken under it. The
    // snapshot shape had a window: a token Mint()ed by the worker between the
    // snapshot and the cancel loop was never cancelled, so a turn that popped
    // in that window ran on unwakeable -- tools parked on its cv slept through
    // the cancel. Mint() takes the same mutex, so holding it through the
    // cancel closes the window. Lock order is one-directional (source.mu_ ->
    // each token's mu_, via Cancel); no path holds a token's mu_ and then
    // takes source.mu_, so no cycle exists.
    void CancelAll() {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& weak : tokens_) {
            if (auto token = weak.lock()) token->Cancel();
        }
    }

private:
    std::mutex mu_;
    std::vector<std::weak_ptr<CancelToken>> tokens_;
};

}  // namespace minimal::tools
