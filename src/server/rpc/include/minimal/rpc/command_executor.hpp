#pragma once

// A small fixed pool for read-only /-commands, replacing detached per-command
// threads. Defects the detached design had, all fixed by the same object:
//
//   - One never-joined thread per command made a flood of read-only commands an
//     unbounded pile of threads. The pool caps both the worker count and the
//     backlog; TryPost refuses beyond them and the caller answers the command
//     with an error rather than dropping a request_id a client is waiting on.
//   - A detached thread outliving the service held a shared_ptr<Session> whose
//     borrowed raw pointers point into registries that are destroyed around it
//     -- a use-after-free window at shutdown. The pool is owned by the service
//     and joined in its destructor, which runs before those registries die.
//   - Its worker could once park forever in a session queue push. The sink
//     never blocks now (a full subscriber drops its own oldest), so a task
//     can always finish.
//
// TWO PROTECTIONS AGAINST A HUNG PLUGIN COMMAND (a plugin command is arbitrary
// code and a thread cannot be killed):
//   - Per-key admission: one session (the key) may hold at most
//     `max_per_key` queued+running tasks, so it cannot occupy every worker.
//   - A deadline: a task running past it is declared timed out -- its
//     `on_timeout` answers the client, a replacement worker restores the pool's
//     capacity, and the stuck thread is left to finish (then exits). At most
//     `max_abandoned` threads may be stuck at once; beyond that TryPost refuses.
//     The late task may still emit its own result after the timeout answer.
//
// Read-only commands must still never run on the gRPC reader thread itself --
// this pool exists so they can run somewhere that is allowed to take a moment
// without stalling the read loop or piling up threads.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "minimal/rpc/policy.hpp"

namespace minimal::rpc {

class CommandExecutor {
public:
    using Task = std::function<void()>;

    struct Options {
        std::size_t max_per_key = kMaxCommandsPerKey;
        std::chrono::milliseconds deadline = kCommandDeadline;  // 0 = no deadline
        std::size_t max_abandoned = kMaxAbandonedCommands;
    };

    CommandExecutor(std::size_t threads, std::size_t max_backlog);
    CommandExecutor(std::size_t threads, std::size_t max_backlog, Options options);
    // Joins every worker. Callers must guarantee no task can outlive what it
    // borrows -- which is why this runs in SessionServiceImpl's destructor,
    // before the registries a command borrows are torn down.
    ~CommandExecutor();

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    // Never blocks. False means the backlog is full, `key` is at its limit, too
    // many threads are stuck, or the pool is shutting down; the caller must
    // answer the command itself. `on_timeout` (optional) runs on the watchdog
    // thread if the task exceeds the deadline; it must not block.
    bool TryPost(Task task, std::string key = {}, Task on_timeout = {});

private:
    struct Item {
        Task task;
        std::string key;
        Task on_timeout;
    };
    // One per worker thread currently inside a task.
    struct Running {
        std::chrono::steady_clock::time_point started;
        std::string key;
        Task on_timeout;
        bool timed_out = false;
    };

    void WorkerMain();
    void WatchdogMain();
    void SpawnWorkerLocked();
    void ReleaseKeyLocked(const std::string& key);
    void JoinFinished();

    Options options_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> items_;
    std::size_t max_backlog_;
    std::size_t abandoned_ = 0;  // stuck threads not yet finished
    std::unordered_map<std::string, std::size_t> per_key_;
    bool stopping_ = false;
    std::list<Running> running_;  // stable addresses for the workers' handles
    std::list<std::thread> workers_;
    // Timed-out workers that have since finished: joined by the watchdog.
    std::list<std::thread> finished_;
    std::thread watchdog_;
    std::condition_variable watchdog_cv_;
};

}  // namespace minimal::rpc
