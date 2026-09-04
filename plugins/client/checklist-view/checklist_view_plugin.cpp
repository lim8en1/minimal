// checklist-view -- the bundled `{todo}` status-bar segment.
//
// Paired with the server's bundled `checklist` plugin (plugins/server/checklist),
// which exposes per-session task lists over the generic PluginCall channel as
// "checklist.rpc" ("get" {"session_id"} -> {"items":[{text,status}]}). This is
// that data's UI, on the same shape as cost-view: the segment renders the
// completed/total counts plus a marker while any task is in progress.
//
// StatusSegment::Render() runs on the render thread with a "cheap, never
// block" contract, so this plugin never calls PluginCall from Render(): a
// background thread polls the focused session every few seconds (immediately
// on a tab switch) and caches the formatted string. A checklist that is empty
// or missing renders as an unfilled segment -- "no tasks" is not "0/0".

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"

namespace {

namespace plugins = minimal::client::plugins;

// nullopt: no checklist yet (fresh session, refused/unreachable call, parse
// failure) -- Render() shows nothing, same as any other unfilled segment.
std::optional<std::string> FormatChecklist(const nlohmann::json& body) {
    if (!body.is_object()) return std::nullopt;
    const auto& items = body.find("items");
    if (items == body.end() || !items->is_array() || items->empty()) return std::nullopt;

    int completed = 0;
    int in_progress = 0;
    for (const auto& item : *items) {
        const std::string status = item.value("status", "pending");
        if (status == "completed") {
            ++completed;
        } else if (status == "in_progress") {
            ++in_progress;
        }
    }
    const std::size_t total = items->size();

    std::string text = "todo " + std::to_string(completed) + "/" + std::to_string(total);
    if (in_progress > 0) text += " (" + std::to_string(in_progress) + " active)";
    return text;
}

class ChecklistStatusSegment final : public plugins::StatusSegment {
public:
    explicit ChecklistStatusSegment(plugins::ClientApi* client_api) : client_api_(client_api) {
        worker_ = std::thread([this] { RunLoop(); });
    }

    ~ChecklistStatusSegment() override {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    ChecklistStatusSegment(const ChecklistStatusSegment&) = delete;
    ChecklistStatusSegment& operator=(const ChecklistStatusSegment&) = delete;

    std::string_view id() const override { return "todo"; }

    // Same tab-switch discipline as cost-view: a switch clears the cache and
    // wakes the worker for an immediate fetch, so the label never shows the
    // previous tab's checklist.
    std::string Render(const std::string& focused_session_id) const override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (focused_session_id != focused_session_id_) {
            focused_session_id_ = focused_session_id;
            cached_text_.clear();
            refresh_requested_ = true;
            cv_.notify_all();
        }
        return cached_text_;
    }

private:
    void RunLoop() {
        std::unique_lock<std::mutex> lock(mtx_);
        while (!stop_) {
            const std::string session_id = focused_session_id_;
            lock.unlock();
            if (!session_id.empty() && client_api_ != nullptr) {
                auto text = Fetch(session_id);
                lock.lock();
                // Only apply if still the same tab -- a slow call answering
                // after the user already switched away must not clobber the
                // new tab's cache.
                if (session_id == focused_session_id_ && text.has_value()) {
                    cached_text_ = std::move(*text);
                }
            } else {
                lock.lock();
            }
            cv_.wait_for(lock, std::chrono::seconds(3),
                         [this] { return stop_ || refresh_requested_; });
            refresh_requested_ = false;
        }
    }

    std::optional<std::string> Fetch(const std::string& session_id) const {
        const nlohmann::json payload{{"session_id", session_id}};
        const auto response = client_api_->PluginCall("checklist", "get", payload.dump());
        if (!response.has_value()) return std::nullopt;  // transport-level: try next poll
        try {
            return FormatChecklist(nlohmann::json::parse(*response));
        } catch (const std::exception&) {
            return std::nullopt;  // a plugin-level error payload, or malformed JSON
        }
    }

    plugins::ClientApi* client_api_;
    std::thread worker_;

    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    mutable std::string focused_session_id_;
    mutable std::string cached_text_;
    bool stop_ = false;
    mutable bool refresh_requested_ = false;
};

}  // namespace

extern "C" {

const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "checklist-view"; }
const char* minimal_client_plugin_version() { return minimal::common::Version(); }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered: degrade to not registering rather than a
    // segment that can never do anything.
    if (context.client_api == nullptr) return;
    registry.Register(std::make_unique<ChecklistStatusSegment>(context.client_api));
}

}  // extern "C"
