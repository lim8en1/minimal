#pragma once

// One worker thread running posted tasks in the order they were posted.
//
// The shape shared by every "queue work behind one draining consumer" in the
// server (hook actions, session-observer notifications): a bounded queue,
// never-blocking posts (the posting threads -- RPC handlers, session workers,
// the reaper -- must not block on a wedged consumer, so a full queue refuses
// instead), a task that throws is logged and does not stop the worker, and an
// outstanding count tests can wait on.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "minimal/common/event_queue.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::common {

class SerialExecutor {
public:
    // `name` labels the warning logged when a task throws.
    SerialExecutor(std::string name, std::size_t capacity, Logger& logger);
    // Stop(Backlog::kRun) if Stop was not called. Must not run on the worker
    // thread (a task destroying its own executor): the worker cannot join
    // itself.
    ~SerialExecutor();

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    // Queues `task` without blocking. False when the queue is full or the
    // executor is stopped; the task is then dropped.
    bool TryPost(std::function<void()> task);

    enum class Backlog { kRun, kDiscard };
    // Refuses further posts, then either runs or discards what is queued, and
    // joins the worker. A task already running always completes. Idempotent.
    // Called from a task (the worker thread) it closes the queue but does not
    // join -- the worker finishes the backlog and the destructor joins.
    void Stop(Backlog backlog);

    bool stopped() const { return tasks_.closed(); }

    // Blocks until every task posted so far has run (or been discarded). Not
    // callable from a task: the calling task is itself outstanding.
    void WaitForIdle();

private:
    void Finished();

    std::string name_;
    Logger& logger_;
    EventQueue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::size_t outstanding_ = 0;  // guarded by mutex_: queued plus running
    std::thread worker_;
    // The live worker's id; cleared once it is joined, because the OS may hand
    // a dead thread's id to a new thread. Atomic: read by any caller.
    std::atomic<std::thread::id> worker_id_;
    std::once_flag stop_once_;
    std::mutex join_mutex_;      // serializes concurrent Stop() joins
};

}  // namespace minimal::common
