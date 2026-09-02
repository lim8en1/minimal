#pragma once

// A bounded job queue with one worker thread, for the unary gRPC calls the TUI
// used to make ON THE LOOP THREAD -- some of them inside a Renderer lambda
// (see app.cpp's frame-assembly comment). RefreshScrollback (2s), SuggestPaths
// (250ms), CreateSession (5s), ListSessions/DeleteSession (2s) all blocked the
// whole UI for up to their deadline on a merely-slow server; two of them ran
// on every frame that changed the transcript version or every keystroke after
// '@'.
//
// This mirrors SessionClient::Stream's own writer thread (session_client.cpp):
// a bounded std::deque, a condition_variable, one worker draining it in order.
// One worker gives FIFO ordering for free, which is why a queued delete can
// never race ahead of a list that was queued after it -- there is no pool to
// reorder them.
//
// Jobs run arbitrary work (typically a blocking unary RPC) off the loop
// thread and are expected to publish their result into AppState/SessionState
// themselves, under those types' own mutexes, and call BumpUiVersion() (or a
// mutator that already does) so the pump's next tick redraws from it -- the
// same contract the reader threads already satisfy. This queue has no
// opinion about what a job does; it only guarantees ordered, off-thread
// execution and a clean join at teardown.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace minimal::client {

class RpcQueue {
public:
    using Job = std::function<void()>;

    // `capacity` bounds the backlog: a queue whose worker is stuck behind a
    // wedged server (no per-job deadline is enforced here -- each job is
    // expected to carry its own, the way every unary call already does) must
    // not grow client memory without limit. A full queue drops the OLDEST
    // job rather than refusing the newest: a stale scrollback-refresh request
    // is worthless once a newer one is queued behind it, and dropping the
    // front keeps Submit() from ever blocking the loop thread -- the one
    // thing this class exists to prevent.
    explicit RpcQueue(std::size_t capacity = 64);
    ~RpcQueue();

    RpcQueue(const RpcQueue&) = delete;
    RpcQueue& operator=(const RpcQueue&) = delete;

    // Enqueues `job` for the worker thread. Never blocks. A no-op after
    // Stop() (or during ~RpcQueue), so a straggling Submit from a callback
    // mid-teardown is silently dropped rather than resurrecting the worker.
    void Submit(Job job);

    // Wakes the worker with no more work coming and joins it. Idempotent.
    // Jobs still queued when Stop() runs are DISCARDED, not drained -- the
    // caller state they'd mutate (AppState/SessionState) may already be
    // torn down by the time Stop() is reachable (RunTui calls it right
    // after the pump joins, before CloseAllTabs).
    void Stop();

private:
    void WorkerMain();

    std::size_t capacity_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace minimal::client
