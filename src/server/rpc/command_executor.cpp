#include "minimal/rpc/command_executor.hpp"

namespace minimal::rpc {

CommandExecutor::CommandExecutor(std::size_t threads, std::size_t max_backlog)
    : max_backlog_(max_backlog) {
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { WorkerMain(); });
    }
}

CommandExecutor::~CommandExecutor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

bool CommandExecutor::TryPost(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= max_backlog_) return false;
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void CommandExecutor::WorkerMain() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stopping_ && drained
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

}  // namespace minimal::rpc
