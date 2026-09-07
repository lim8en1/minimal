// cost-view -- the bundled `{cost}` status-bar segment.
//
// Paired with the server's bundled `cost-tracker` plugin (plugins/server/
// cost-tracker): that plugin already computes real cumulative $ costs and
// per-model token totals per session, and exposes them over the generic
// PluginCall channel as "cost-tracker.rpc" specifically so a UI could consume
// them -- this is that UI, the thinnest reasonable client for it.
//
// StatusSegment::Render() runs on the render thread at ~20 Hz with a "cheap,
// never block" contract (registries.hpp), so this plugin never calls
// PluginCall from Render() itself: a small background thread polls
// "cost-tracker.rpc"/"totals" for the currently-focused session every few
// seconds (plus immediately on a tab switch) and caches the formatted
// string; Render() only ever reads the cache.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"
#include "cost_view_format.hpp"

namespace {

namespace plugins = minimal::client::plugins;
using minimal::client::cost_view::FormatTotals;   // pure formatters, see cost_view_format.hpp

// The formatters live in cost_view_format.hpp (pure, header-only) so they
// are testable -- the logo plugin's pure-extraction pattern. This TU only
// wires them into the status segment.


class CostStatusSegment final : public plugins::StatusSegment {
public:
    explicit CostStatusSegment(plugins::ClientApi* client_api) : client_api_(client_api) {
        worker_ = std::thread([this] { RunLoop(); });
    }

    ~CostStatusSegment() override {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        // The join can wait out one in-flight fetch, bounded by
        // SessionClient::PluginCall's 5 s deadline. Deliberate: a detached
        // worker would keep calling into the ClientApi (and the
        // SessionClient behind it) while this destructor tears the object
        // down -- a shutdown UAF is strictly worse than a bounded stall.
        if (worker_.joinable()) worker_.join();
    }

    CostStatusSegment(const CostStatusSegment&) = delete;
    CostStatusSegment& operator=(const CostStatusSegment&) = delete;

    std::string_view id() const override { return "cost"; }

    std::string Render(const std::string& focused_session_id) const override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (focused_session_id != focused_session_id_) {
            // A tab switch: wake the worker for an immediate refresh instead
            // of showing the PREVIOUS tab's totals for up to a whole poll
            // interval under the new one's label. refresh_requested_ is what
            // actually wakes it -- wait_for's predicate used to be
            // `return stop_;`, so this notify landed while the worker was
            // still inside its 3 s wait with nothing telling it to stop
            // waiting, and the segment sat blank (cached_text_ was just
            // cleared) until the poll interval happened to elapse on its own.
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
                // new tab's (possibly already-refreshed) cache.
                if (session_id == focused_session_id_ && text.has_value()) {
                    cached_text_ = std::move(*text);
                }
            } else {
                lock.lock();
            }
            cv_.wait_for(lock, std::chrono::seconds(3),
                        [this] { return stop_ || refresh_requested_; });
            refresh_requested_ = false;   // consumed regardless of why the wait ended
        }
    }

    std::optional<std::string> Fetch(const std::string& session_id) const {
        const nlohmann::json payload{{"session_id", session_id}};
        const auto response = client_api_->PluginCall("cost-tracker", "totals", payload.dump());
        if (!response.has_value()) return std::nullopt;   // transport-level: try again next poll
        try {
            return FormatTotals(nlohmann::json::parse(*response));
        } catch (const std::exception&) {
            return std::nullopt;   // a plugin-level error payload, or malformed JSON
        }
    }

    plugins::ClientApi* client_api_;
    std::thread worker_;

    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    mutable std::string focused_session_id_;
    mutable std::string cached_text_;
    bool stop_ = false;
    // Set by Render() on a tab switch, under mtx_ (mutable: Render() is
    // const). Wakes RunLoop's wait_for immediately instead of the notify
    // landing with nothing in the predicate to act on it -- see Render()'s
    // comment.
    mutable bool refresh_requested_ = false;
};

}  // namespace

extern "C" {

const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "cost-view"; }
const char* minimal_client_plugin_version() { return minimal::common::Version(); }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered (e.g. a host that hasn't wired one, or a
    // test fixture exercising other registries): degrade to not registering
    // rather than a segment that can never do anything.
    if (context.client_api == nullptr) return;
    registry.Register(std::make_unique<CostStatusSegment>(context.client_api));
}

}  // extern "C"
