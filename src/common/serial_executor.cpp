#include "minimal/common/serial_executor.hpp"

#include <exception>
#include <utility>

namespace minimal::common {

SerialExecutor::SerialExecutor(std::string name, std::size_t capacity, Logger& logger)
    : name_(std::move(name)), logger_(logger), tasks_(capacity) {
    worker_ = std::thread([this] {
        while (auto task = tasks_.Pop()) {
            try {
                (*task)();
            } catch (const std::exception& error) {
                logger_.warn("{} task threw: {}", name_, error.what());
            } catch (...) {
                logger_.warn("{} task threw a non-standard exception", name_);
            }
            Finished();
        }
    });
}

SerialExecutor::~SerialExecutor() { Stop(Backlog::kRun); }

bool SerialExecutor::TryPost(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++outstanding_;
    }
    if (tasks_.TryPush(std::move(task))) return true;
    Finished();  // never queued, so never finishes on its own
    return false;
}

void SerialExecutor::Stop(Backlog backlog) {
    std::call_once(stop_once_, [&] {
        tasks_.Close();
        if (backlog == Backlog::kDiscard) {
            const std::size_t dropped = tasks_.DiscardPending();
            std::lock_guard<std::mutex> lock(mutex_);
            outstanding_ -= dropped;
        }
        if (worker_.joinable()) worker_.join();
        idle_cv_.notify_all();
    });
}

void SerialExecutor::WaitForIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return outstanding_ == 0; });
}

void SerialExecutor::Finished() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --outstanding_;
    }
    idle_cv_.notify_all();
}

}  // namespace minimal::common
