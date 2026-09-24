#include "totals_hub.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::cost_tracker {

TotalsHub::TotalsHub(SessionIndex& index, Compute compute, std::chrono::milliseconds heartbeat,
                     std::chrono::milliseconds debounce)
    : index_(index), compute_(std::move(compute)), heartbeat_(heartbeat), debounce_(debounce) {}

TotalsHub::~TotalsHub() { Shutdown(); }

void TotalsHub::AddSink(std::shared_ptr<plugins::PluginSubscriptionSink> sink) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stop_ || sink == nullptr) return;
    sinks_.push_back(std::move(sink));
    // Started here, not in the constructor: every member Run() touches exists
    // by now, and a server nobody subscribes to never pays for the thread.
    if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
}

void TotalsHub::MarkDirty(const std::string& session_id) {
    if (session_id.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_ || sinks_.empty()) return;
        dirty_.insert(session_id);
    }
    cv_.notify_all();
}

void TotalsHub::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mtx_);
    sinks_.clear();
}

void TotalsHub::Run() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stop_) {
        cv_.wait_for(lock, heartbeat_, [this] { return stop_ || !dirty_.empty(); });
        if (stop_) break;

        std::vector<std::string> payloads;
        if (dirty_.empty()) {
            payloads.push_back(R"({"type":"heartbeat"})");
        } else {
            // Debounce: a turn lands as a burst (rounds, then the record) --
            // let it settle so each session recomputes once, not per event.
            cv_.wait_for(lock, debounce_, [this] { return stop_; });
            if (stop_) break;
            std::set<std::string> dirty = std::move(dirty_);
            dirty_.clear();
            lock.unlock();

            // Ancestor expansion here, off the observer thread: the index's
            // first query may load every blob once (session_index.hpp).
            std::set<std::string> expanded;
            for (const auto& id : dirty) {
                expanded.insert(id);
                for (auto& ancestor : index_.Ancestors(id)) expanded.insert(std::move(ancestor));
            }
            for (const auto& id : expanded) {
                try {
                    nlohmann::json push = compute_(id).value_or(nlohmann::json{{"turns", 0}});
                    push["type"] = "totals";
                    push["session_id"] = id;
                    payloads.push_back(push.dump());
                } catch (const std::exception& error) {
                    minimal::common::DefaultLogger().warn(
                        "cost-tracker: computing pushed totals for session {} failed: {}", id,
                        error.what());
                }
            }
            lock.lock();
            if (stop_) break;
        }

        auto sinks = sinks_;
        lock.unlock();
        std::vector<plugins::PluginSubscriptionSink*> dead;
        for (const auto& sink : sinks) {
            for (const auto& payload : payloads) {
                if (!sink->Push(payload)) {
                    dead.push_back(sink.get());
                    break;
                }
            }
        }
        lock.lock();
        if (!dead.empty()) {
            std::erase_if(sinks_, [&dead](const auto& sink) {
                return std::find(dead.begin(), dead.end(), sink.get()) != dead.end();
            });
        }
    }
}

}  // namespace minimal::cost_tracker
