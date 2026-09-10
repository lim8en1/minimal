#include "minimal/client/rpc_queue.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client {

RpcQueue::RpcQueue(std::size_t capacity) : capacity_(capacity) {
    worker_ = std::thread([this] { WorkerMain(); });
}

RpcQueue::~RpcQueue() { Stop(); }

void RpcQueue::Submit(Job job, const bool must_run) {
    if (!job) return;
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return;   // dropped: no worker left to run it
        if (queue_.size() >= capacity_) {
            // Drop-oldest, but never a must-run job: it resolves a latch whose
            // setter has already fired and whose only clearer is the job itself
            // -- evicting it latched refresh_pending / older_fetch_pending_
            // forever (refresh_seq is only bumped by a run, never by a drop).
            const auto droppable = std::find_if(queue_.begin(), queue_.end(),
                                                [](const Item& item) { return !item.must_run; });
            if (droppable != queue_.end()) {
                queue_.erase(droppable);   // oldest droppable job is the stalest -- see header
            }
            // Every queued job is must-run: push anyway (transient overrun --
            // see Submit's comment) rather than strand a latch. The queue
            // drains as soon as the worker's current job returns.
        }
        queue_.push_back(Item{std::move(job), must_run});
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

bool RpcQueue::Busy() const {
    std::lock_guard lk(mtx_);
    return running_job_ || !queue_.empty();
}

void RpcQueue::WorkerMain() {
    while (true) {
        Job job;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;   // Stop() already cleared queue_; nothing left to run
            job = std::move(queue_.front().job);
            queue_.pop_front();
            running_job_ = true;
        }
        // Contained, not propagated: a throwing job (EnsureSession, a
        // scrollback fetch, a sessionless command -- all arbitrary
        // downstream code) escaping this call would std::terminate the
        // worker thread, taking the whole client down for a single bad job.
        try {
            job();   // off the loop thread; the job publishes its own result
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("rpc queue: job threw: {}", error.what());
        } catch (...) {
            common::DefaultLogger().warn("rpc queue: job threw a non-standard exception");
        }
        {
            std::lock_guard lk(mtx_);
            running_job_ = false;
        }
    }
}

}  // namespace minimal::client
