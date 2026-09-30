#include "totals_feed.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "cost_view_format.hpp"

namespace minimal::client::cost_view {

namespace {

constexpr char kPluginName[] = "cost-tracker";
constexpr char kMethod[] = "totals";

}  // namespace

TotalsFeed::TotalsFeed(plugins::ClientApi* api, plugins::FocusProvider* focus, Timing timing)
    : api_(api), focus_(focus), timing_(timing) {
    // Constructor BODY, not an initializer -- see background_poller.hpp.
    Start([this] { RunLoop(); });
}

TotalsFeed::~TotalsFeed() {
    // Explicit, not left to ~BackgroundPoller(): see background_poller.hpp's
    // TEARDOWN note for why the derived destructor must call Stop() itself.
    Stop();
}

std::string TotalsFeed::Render(const std::string& focused_session_id, TotalsField field) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (focused_session_id != focused_session_id_) {
        focused_session_id_ = focused_session_id;
        refresh_requested_ = true;
        cv_.notify_all();
    }
    const auto it = cache_.find(focused_session_id);
    if (it == cache_.end()) return {};
    const std::string& text = field == TotalsField::kCost ? it->second.cost : it->second.detail;
    if (text.empty() || !it->second.stale) return text;
    return text + kStaleSuffix;
}

void TotalsFeed::RunLoop() {
    if (api_ == nullptr) return;   // nothing can ever fill; Stop() just joins
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
        }
        api_->PluginSubscribe(kPluginName, kMethod, "{}",
                              [this](const std::string& payload) { return OnPayload(payload); });
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
            // Whatever ended the stream, nothing cached is being kept
            // current any more.
            for (auto& [id, entry] : cache_) entry.stale = true;
        }
        PollFallback();
    }
}

bool TotalsFeed::OnPayload(const std::string& json_payload) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(json_payload);
    } catch (const std::exception&) {
        // Malformed push: skip it, but still run the seed check below.
    }
    const std::string type =
        message.is_object() ? SafeValue<std::string>(message, "type", "") : std::string();

    std::string focused;
    bool seed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_) return false;
        if (type == "totals") {
            const auto session_id = SafeValue<std::string>(message, "session_id", "");
            if (!session_id.empty()) ApplyLocked(session_id, message);
        }
        focused = focused_session_id_;
        refresh_requested_ = false;
        if (!focused.empty()) {
            const auto it = cache_.find(focused);
            seed = it == cache_.end() || it->second.stale;
        }
    }

    if (type == "heartbeat") EvictClosedTabs();

    if (seed) {
        const auto totals = Fetch(focused);
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_) return false;
        if (totals.has_value()) {
            ApplyLocked(focused, *totals);
        } else {
            // Nothing recorded yet (the server refuses "totals" for a
            // session with no turns) or the call failed: an empty, CURRENT
            // entry, so the next heartbeat does not re-seed. The stream's
            // pushes fill it once the session spends anything.
            cache_[focused] = Entry{};
        }
    }
    RequestRedraw();   // outside mtx_: the host's seam is host code
    return true;
}

void TotalsFeed::PollFallback() {
    const auto deadline = std::chrono::steady_clock::now() + timing_.fallback_period;
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
        const std::string session_id = focused_session_id_;
        refresh_requested_ = false;
        if (!session_id.empty()) {
            lock.unlock();
            const auto totals = Fetch(session_id);
            lock.lock();
            if (stop_) return;
            if (totals.has_value()) {
                ApplyLocked(session_id, *totals);
                lock.unlock();
                RequestRedraw();   // outside mtx_
                lock.lock();
                if (stop_) return;
            } else if (const auto it = cache_.find(session_id); it != cache_.end()) {
                it->second.stale = true;
            }
        }
        const auto wake = std::min(deadline, std::chrono::steady_clock::now() + timing_.poll_interval);
        cv_.wait_until(lock, wake, [this] { return stop_ || refresh_requested_; });
    }
}

void TotalsFeed::ApplyLocked(const std::string& session_id, const nlohmann::json& totals) {
    Entry entry;
    entry.cost = FormatTotals(totals).value_or("");
    entry.detail = FormatTokenDetail(totals).value_or("");
    cache_[session_id] = std::move(entry);
}

void TotalsFeed::EvictClosedTabs() {
    if (focus_ == nullptr) return;
    // Outside mtx_: the provider is host code; never call out under our lock.
    std::set<std::string> open;
    for (const auto& tab : focus_->Tabs()) open.insert(tab.session_id);
    std::lock_guard<std::mutex> lock(mtx_);
    open.insert(focused_session_id_);
    std::erase_if(cache_, [&open](const auto& item) { return open.count(item.first) == 0; });
}

std::optional<nlohmann::json> TotalsFeed::Fetch(const std::string& session_id) const {
    const nlohmann::json payload{{"session_id", session_id}};
    const auto response = api_->PluginCall(kPluginName, kMethod, payload.dump());
    if (!response.has_value()) return std::nullopt;   // refused, unknown session, or transport
    try {
        return nlohmann::json::parse(*response);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace minimal::client::cost_view
