#pragma once

// The per-session tool-dispatch pool (ARCHITECTURE.md "Tool dispatch").
//
// Fixed-width, lazily started, owned by the session's AgentLoop. Parallel-safe
// tool calls of one round run here while the session worker gates the next
// round (or runs its serial-only calls); the worker joins via Wait() before it
// appends results.
//
// THREAD CONTRACT -- the three properties the termination ordering depends on:
//
// 1. Tasks NEVER block on the permission checker or any PendingRequests
//    primitive. Gate-then-run guarantees it: by the time a call is submitted
//    its permission decision is a value in the PreparedInvocation. This is what
//    keeps Session::Terminate's fixed order (PendingRequests::Shutdown ->
//    EventQueue::Close -> join the worker, which joins this pool) deadlock-free.
// 2. Tasks only block on tool execution, which is deadline-bounded (bash's
//    timeout, the MCP request deadline, the shared CancelToken) -- so a cancel
//    or teardown always drains the pool in bounded time.
// 3. Tasks own everything they touch (by value / shared_ptr / references that
//    provably outlive Wait()'s return -- the caller's stack). Same rule as
//    tests/support/deadline.hpp: a callable that borrows a shorter lifetime is
//    a use-after-free, not a hang.
//
// Width 1 degenerates to "run inline": no threads are created and Submit runs
// the task on the calling thread, so the sequential path allocates nothing.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace minimal::tools {

class ToolPool {
public:
    // width <= 0 is clamped to 1 by the loader; anything <= 1 here means
    // "never start threads".
    explicit ToolPool(int width);
    // Signals shutdown and joins every worker. Must not race Submit/Wait --
    // in practice the agent loop has already Wait()ed, and destruction happens
    // on the session's worker after its last round.
    ~ToolPool();

    ToolPool(const ToolPool&) = delete;
    ToolPool& operator=(const ToolPool&) = delete;

    // Submits one task. Blocks the CALLER while width tasks are queued or
    // running -- that is the width limit, and the caller is the session worker,
    // which is permitted to block. Inline mode (width <= 1) runs immediately.
    void Submit(std::function<void()> task);

    // Blocks until every submitted task has finished. Idempotent.
    void Wait();

    int width() const { return width_; }

private:
    void WorkerMain();

    int width_;
    std::vector<std::thread> workers_;  // started lazily on first Submit
    std::mutex mutex_;
    std::condition_variable task_cv_;   // workers wait for tasks
    std::condition_variable done_cv_;   // Submit (backpressure) and Wait wait here
    std::deque<std::function<void()>> tasks_;
    std::size_t running_ = 0;   // tasks currently executing
    bool shutdown_ = false;
};

}  // namespace minimal::tools
