#pragma once

// A small fixed pool for read-only /-commands, replacing detached per-command
// threads. Three defects the detached design had, all fixed by the same object:
//
//   - One never-joined thread per command made a flood of read-only commands an
//     unbounded pile of threads. The pool caps both the worker count and the
//     backlog; TryPost refuses beyond them and the caller answers the command
//     with an error rather than dropping a request_id a client is waiting on.
//   - A detached thread outliving the service held a shared_ptr<Session> whose
//     borrowed raw pointers point into registries that are destroyed around it
//     -- a use-after-free window at shutdown. The pool is owned by the service
//     and joined in its destructor, which runs before those registries die.
//   - Its worker could park forever in the session queue's blocking Push. The
//     command path now uses TryPush, so a task can always finish.
//
// Read-only commands must still never run on the gRPC reader thread itself --
// this pool exists so they can run somewhere that is allowed to take a moment
// without stalling the read loop or piling up threads.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace minimal::rpc {

class CommandExecutor {
public:
    using Task = std::function<void()>;

    CommandExecutor(std::size_t threads, std::size_t max_backlog);
    // Joins every worker. Callers must guarantee no task can outlive what it
    // borrows -- which is why this runs in SessionServiceImpl's destructor,
    // before the registries a command borrows are torn down.
    ~CommandExecutor();

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    // Never blocks. False means the backlog is full or the pool is shutting
    // down; the caller must answer the command itself.
    bool TryPost(Task task);

private:
    void WorkerMain();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::size_t max_backlog_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace minimal::rpc
