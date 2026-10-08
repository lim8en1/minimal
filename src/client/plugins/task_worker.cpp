#include "minimal/client/sdk/task_worker.hpp"

#include <exception>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {

TaskWorker::TaskWorker(std::string name, std::function<void()> after_each)
    : name_(std::move(name)), after_each_(std::move(after_each)) {}

TaskWorker::~TaskWorker() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
        tasks_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TaskWorker::Post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_) return;
        tasks_.push_back(std::move(task));
        // Started on the first task, not at construction: every view owns a
        // worker, and most views are never opened -- a thread each, parked
        // for the life of the process, bought nothing.
        if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    }
    cv_.notify_one();
}

void TaskWorker::Run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("{}: {}", name_, error.what());
        } catch (...) {
            common::DefaultLogger().warn("{}: a task threw a non-standard exception", name_);
        }
        if (after_each_) after_each_();
    }
}

}  // namespace minimal::client::plugins
