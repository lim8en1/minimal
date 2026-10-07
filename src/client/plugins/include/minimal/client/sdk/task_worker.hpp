#pragma once

// A serial background queue for a client plugin's blocking calls --
// PluginCall, ClientApi reads -- which must never run on the UI thread.
// Post() from any thread; tasks run in order, one at a time, on the worker's
// own thread; `after_each` (typically HostUi::RequestRedraw: the frame only
// repaints when something asks) runs after every task, outside any lock. A
// task that throws is logged under the worker's name and the queue goes on.
//
// LIFETIME: own it as a member declared LAST. Members are destroyed in
// reverse order, so the worker -- whose destructor drops what is still
// queued and joins after the running task -- goes first, while everything
// its tasks touch still exists. Unlike BackgroundPoller there is nothing for
// the owner to call: the thread, its lock and its queue are all the
// worker's own, constructed before the thread starts.
//
// For a periodic poll (a feed refreshing every few seconds) use
// BackgroundPoller; this is for one-off requests in order.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace minimal::client::plugins {

class TaskWorker {
public:
    // `name` labels a contained throw in the log ("config-view").
    explicit TaskWorker(std::string name, std::function<void()> after_each = {});
    ~TaskWorker();
    TaskWorker(const TaskWorker&) = delete;
    TaskWorker& operator=(const TaskWorker&) = delete;

    // Queues `task`; a no-op once the worker is being destroyed.
    void Post(std::function<void()> task);

private:
    void Run();

    std::string name_;
    std::function<void()> after_each_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false;
    std::thread thread_;  // last: starts after every member above exists
};

}  // namespace minimal::client::plugins
