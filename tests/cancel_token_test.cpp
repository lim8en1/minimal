#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "minimal/tools/cancel_token.hpp"

using minimal::tools::CancelSource;
using minimal::tools::CancelToken;
using namespace std::chrono_literals;

namespace {
// Holds a token's mutex for a while, stretching CancelAll's cancel of that
// token -- which is what opens the window the invariant below guards.
struct MutexHolder {
    explicit MutexHolder(CancelToken& token)
        : lock(token.Mu()) {}
    std::lock_guard<std::mutex> lock;
};
}  // namespace

TEST_CASE("CancelAll cancels every token minted before it returns", "[cancel_token][invariant]") {
    // THE INVARIANT: a token Mint()ed while CancelAll is running must be
    // cancelled by it. The snapshot-and-cancel-outside shape had a window --
    // a Mint between the snapshot and the cancel loop produced a token that
    // nobody ever cancelled, so the turn carrying it slept through the cancel
    // on a cv nobody would wake. Holding the registry mutex through the
    // cancel closes the window: with the fix, Mint either lands before the
    // cancel (and is covered) or blocks until it is done.
    CancelSource source;
    auto early = source.Mint();

    // A waiter holding the token's mutex through the cancel stretches
    // CancelAll's execution, so the Mint below genuinely races it rather than
    // landing before or after the whole call.
    std::atomic<bool> holder_done{false};
    std::thread holder([&] {
        MutexHolder held(*early);
        // Bounded: under the fixed code the holder's release is what lets
        // CancelAll (and therefore the blocked Mint below) finish, so this
        // must not wait on holder_done forever -- the deadline breaks the
        // circular wait the test itself constructs.
        const auto deadline = std::chrono::steady_clock::now() + 300ms;
        while (!holder_done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
    });
    // Wait until the holder actually owns the mutex: once it does, CancelAll
    // is provably blocked inside its cancel loop (or about to be).
    std::this_thread::sleep_for(20ms);

    std::atomic<int> order{0};  // bit 1: Mint finished first; bit 2: CancelAll first
    auto cancel = std::thread([&] {
        source.CancelAll();
        order.fetch_or(2);
    });
    std::this_thread::sleep_for(20ms);  // let CancelAll reach the blocked token
    auto late = source.Mint();
    order.fetch_or(1);
    holder_done.store(true);
    cancel.join();
    holder.join();

    // If Mint returned before CancelAll did, the token it minted must have
    // been cancelled by that same CancelAll.
    if (order.load() == 1) {
        CHECK(late->IsCancelled());
    }
    // Either way, the token that existed before the call is cancelled.
    CHECK(early->IsCancelled());
}

TEST_CASE("a token minted after CancelAll returns belongs to the next turn",
          "[cancel_token]") {
    // The complement of the invariant above: cancelling everything live must
    // not poison tokens minted afterwards -- the next turn starts clean.
    CancelSource source;
    source.CancelAll();
    auto fresh = source.Mint();
    CHECK_FALSE(fresh->IsCancelled());
}
