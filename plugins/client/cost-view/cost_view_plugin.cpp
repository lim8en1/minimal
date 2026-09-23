// cost-view -- the bundled `{cost}` and `{token_detail}` status-bar segments.
//
// Paired with the server's bundled `cost-tracker` plugin (plugins/server/
// cost-tracker): that plugin already computes real cumulative $ costs and
// per-model token totals (input/output plus cache read/write) per session,
// and exposes them over the generic PluginCall channel as
// "cost-tracker.rpc"/"totals" specifically so a UI could consume them -- this
// is that UI, the thinnest reasonable client for it. One PluginCall per poll
// feeds BOTH segments: {cost} renders the dollar figure, {token_detail} the
// four token sums (the formatters decide; see cost_view_format.hpp).
//
// StatusSegment::Render() runs on the render thread at ~20 Hz with a "cheap,
// never block" contract (registries.hpp), so the segments never call
// PluginCall from Render() themselves: a single background thread polls
// "cost-tracker.rpc"/"totals" for the currently-focused session every few
// seconds (plus immediately on a tab switch) and caches the formatted
// strings; Render() only ever reads the cache.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/client/plugins/background_poller.hpp"
#include "minimal/common/abi_tag.hpp"
#include "cost_view_format.hpp"

namespace {

namespace plugins = minimal::client::plugins;
using minimal::client::cost_view::FormatTokenDetail;   // pure formatters, see cost_view_format.hpp
using minimal::client::cost_view::FormatTotals;

// The formatters live in cost_view_format.hpp (pure, header-only) so they
// are testable -- the logo plugin's pure-extraction pattern. This TU only
// wires them into the status segments.

// Shared polling state behind BOTH segments: one worker thread, one
// PluginCall per poll, two cached strings. Two independent pollers would
// double the PluginCall traffic for data a single reply already carries.
class TotalsPoller : private plugins::BackgroundPoller {
public:
    explicit TotalsPoller(plugins::ClientApi* client_api) : client_api_(client_api) {
        // Constructor BODY, not an initializer -- see background_poller.hpp.
        Start([this] { RunLoop(); });
    }

    ~TotalsPoller() {
        // Explicit, not left to ~BackgroundPoller(): see checklist_poller.cpp's
        // ~ChecklistPoller for why the derived destructor must call Stop()
        // itself. The join can wait out one in-flight fetch, bounded by
        // SessionClient::PluginCall's 5 s deadline. Deliberate: a detached
        // worker would keep calling into the ClientApi (and the
        // SessionClient behind it) while this destructor tears the object
        // down -- a shutdown UAF is strictly worse than a bounded stall.
        Stop();
    }

    TotalsPoller(const TotalsPoller&) = delete;
    TotalsPoller& operator=(const TotalsPoller&) = delete;

    // `detail` selects which cached string the caller renders ({cost} vs
    // {token_detail}); the tab-switch wake logic is shared -- a switch is
    // detected once per frame (the two segments Render back to back on the
    // render thread), the second caller sees the updated focus and just
    // reads the freshly cleared cache.
    std::string Render(const std::string& focused_session_id, bool detail) const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (focused_session_id != focused_session_id_) {
            // A tab switch: wake the worker for an immediate refresh instead
            // of showing the PREVIOUS tab's totals for up to a whole poll
            // interval under the new one's label. refresh_requested_ is what
            // actually wakes it -- wait_for's predicate used to be
            // `return stop_;`, so this notify landed while the worker was
            // still inside its 3 s wait with nothing telling it to stop
            // waiting, and the segment sat blank (the caches were just
            // cleared) until the poll interval happened to elapse on its own.
            focused_session_id_ = focused_session_id;
            cost_text_.clear();
            detail_text_.clear();
            refresh_requested_ = true;
            cv_.notify_all();
        }
        return detail ? detail_text_ : cost_text_;
    }

private:
    void RunLoop() {
        std::unique_lock<std::mutex> lock(mtx_);
        while (!stop_) {
            const std::string session_id = focused_session_id_;
            lock.unlock();
            if (!session_id.empty() && client_api_ != nullptr) {
                const auto totals = Fetch(session_id);
                std::optional<std::string> cost;
                std::optional<std::string> detail;
                if (totals.has_value()) {
                    cost = FormatTotals(*totals);
                    detail = FormatTokenDetail(*totals);
                }
                lock.lock();
                // Only apply if still the same tab -- a slow call answering
                // after the user already switched away must not clobber the
                // new tab's (possibly already-refreshed) caches. A formatter
                // returning nullopt (nothing worth rendering) leaves the
                // previous string in place, the same contract a failed fetch
                // has always had.
                if (session_id == focused_session_id_) {
                    if (cost.has_value()) cost_text_ = std::move(*cost);
                    if (detail.has_value()) detail_text_ = std::move(*detail);
                }
            } else {
                lock.lock();
            }
            cv_.wait_for(lock, std::chrono::seconds(3),
                        [this] { return stop_ || refresh_requested_; });
            refresh_requested_ = false;   // consumed regardless of why the wait ended
        }
    }

    std::optional<nlohmann::json> Fetch(const std::string& session_id) const {
        const nlohmann::json payload{{"session_id", session_id}};
        const auto response = client_api_->PluginCall("cost-tracker", "totals", payload.dump());
        if (!response.has_value()) return std::nullopt;   // transport-level: try again next poll
        try {
            return nlohmann::json::parse(*response);
        } catch (const std::exception&) {
            return std::nullopt;   // a plugin-level error payload, or malformed JSON
        }
    }

    plugins::ClientApi* client_api_;

    // mtx_/cv_/stop_ live in BackgroundPoller (protected base members),
    // constructed before any member below -- see background_poller.hpp.
    mutable std::string focused_session_id_;
    mutable std::string cost_text_;
    mutable std::string detail_text_;
    // Set by Render() on a tab switch, under mtx_ (mutable: Render() is
    // const). Wakes RunLoop's wait_for immediately instead of the notify
    // landing with nothing in the predicate to act on it -- see Render()'s
    // comment.
    mutable bool refresh_requested_ = false;
};

// The two thin segments: id + which cached string they read. All state and
// machinery lives in the shared TotalsPoller.

class CostStatusSegment final : public plugins::StatusSegment {
public:
    explicit CostStatusSegment(std::shared_ptr<TotalsPoller> poller)
        : poller_(std::move(poller)) {}

    std::string_view id() const override { return "cost"; }

    std::string Render(const std::string& focused_session_id) const override {
        return poller_->Render(focused_session_id, /*detail=*/false);
    }

private:
    std::shared_ptr<TotalsPoller> poller_;
};

class TokenDetailStatusSegment final : public plugins::StatusSegment {
public:
    explicit TokenDetailStatusSegment(std::shared_ptr<TotalsPoller> poller)
        : poller_(std::move(poller)) {}

    std::string_view id() const override { return "token_detail"; }

    std::string Render(const std::string& focused_session_id) const override {
        return poller_->Render(focused_session_id, /*detail=*/true);
    }

private:
    std::shared_ptr<TotalsPoller> poller_;
};

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("cost-view")

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No client_api seam offered (e.g. a host that hasn't wired one, or a
    // test fixture exercising other registries): degrade to not registering
    // rather than segments that can never do anything.
    if (context.client_api == nullptr) return;
    // shared_ptr: the LAST segment destroyed tears the poller (and its
    // worker thread) down -- the registry owns the segments, and neither
    // outliving nor underliving the other is expressible this way.
    auto poller = std::make_shared<TotalsPoller>(context.client_api);
    registry.Register(std::make_unique<CostStatusSegment>(poller));
    registry.Register(std::make_unique<TokenDetailStatusSegment>(poller));
}

}  // extern "C"
