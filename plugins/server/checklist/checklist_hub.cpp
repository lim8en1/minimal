#include "checklist_hub.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::checklist {

ChecklistHub::ChecklistHub(ChecklistStore& store, std::chrono::milliseconds heartbeat,
                           std::chrono::milliseconds debounce)
    : store_(store), heartbeat_(heartbeat), debounce_(debounce) {}

ChecklistHub::~ChecklistHub() { Shutdown(); }

void ChecklistHub::AddSink(std::shared_ptr<plugins::PluginSubscriptionSink> sink) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stop_ || sink == nullptr) return;
    sinks_.push_back(std::move(sink));
    // Started here, not in the constructor: every member Run() touches exists
    // by now, and a server nobody subscribes to never pays for the thread.
    if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
}

void ChecklistHub::MarkDirty(const std::string& session_id) {
    if (session_id.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_ || sinks_.empty()) return;
        dirty_.insert(session_id);
    }
    cv_.notify_all();
}

void ChecklistHub::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mtx_);
    sinks_.clear();
}

void ChecklistHub::Run() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stop_) {
        cv_.wait_for(lock, heartbeat_, [this] { return stop_ || !dirty_.empty(); });
        if (stop_) break;

        std::vector<std::string> payloads;
        if (dirty_.empty()) {
            payloads.push_back(R"({"type":"heartbeat"})");
        } else {
            // Debounce: a round of tool calls lands as a burst (set, then
            // updates) -- let it settle so each session loads once.
            cv_.wait_for(lock, debounce_, [this] { return stop_; });
            if (stop_) break;
            std::set<std::string> dirty = std::move(dirty_);
            dirty_.clear();
            lock.unlock();

            for (const auto& id : dirty) {
                try {
                    payloads.push_back(nlohmann::json({{"type", "checklist"},
                                                       {"session_id", id},
                                                       {"items", ItemsJson(store_.Load(id))}})
                                           .dump());
                } catch (const std::exception& error) {
                    minimal::common::DefaultLogger().warn(
                        "checklist: pushing the list for session {} failed: {}", id,
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

}  // namespace minimal::checklist
