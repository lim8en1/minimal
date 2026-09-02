#include "minimal/client/rpc_queue.hpp"

#include <utility>

namespace minimal::client {

RpcQueue::RpcQueue(std::size_t capacity) : capacity_(capacity) {
    worker_ = std::thread([this] { WorkerMain(); });
}

RpcQueue::~RpcQueue() { Stop(); }

void RpcQueue::Submit(Job job) {
    if (!job) return;
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return;   // dropped: no worker left to run it
        if (queue_.size() >= capacity_) {
            queue_.pop_front();   // oldest job is the stalest -- see header
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void RpcQueue::Stop() {
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return;   // idempotent
        stopping_ = true;
        queue_.clear();   // discard, not drain -- see header
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void RpcQueue::WorkerMain() {
    while (true) {
        Job job;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;   // Stop() already cleared queue_; nothing left to run
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();   // off the loop thread; the job publishes its own result
    }
}

}  // namespace minimal::client
