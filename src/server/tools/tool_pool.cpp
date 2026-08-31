#include "minimal/tools/tool_pool.hpp"

namespace minimal::tools {

ToolPool::ToolPool(int width) : width_(width < 1 ? 1 : width) {}

ToolPool::~ToolPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    task_cv_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

void ToolPool::WorkerMain() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            task_cv_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // shutdown with nothing left: workers exit
            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++running_;
        }
        task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
        }
        // Both sleepers care: Wait waits for quiescence, Submit waits for a
        // free slot under the width cap.
        done_cv_.notify_all();
    }
}

void ToolPool::Submit(std::function<void()> task) {
    if (width_ <= 1) {
        // Sequential width: no threads, no queue, exactly the old behavior.
        task();
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // Lazily start workers on first use: a session that never runs a parallel
    // round allocates none.
    if (workers_.empty()) {
        workers_.reserve(static_cast<std::size_t>(width_));
        for (int i = 0; i < width_; ++i) {
            workers_.emplace_back([this] { WorkerMain(); });
        }
    }
    // Backpressure == the width limit. tasks_ + running_ never exceeds width_,
    // so a round wider than the pool makes the worker park here until a task
    // finishes -- which is correct: the worker has nothing better to do, and
    // unbounded queueing would silently defeat the configured cap.
    done_cv_.wait(lock,
                  [this] { return tasks_.size() + running_ < static_cast<std::size_t>(width_); });
    tasks_.push_back(std::move(task));
    task_cv_.notify_one();
}

void ToolPool::Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this] { return tasks_.empty() && running_ == 0; });
}

}  // namespace minimal::tools
