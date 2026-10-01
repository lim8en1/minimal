#include "checklist_feed.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::checklist_view {

namespace {

constexpr char kPluginName[] = "checklist";
constexpr char kStreamMethod[] = "changes";
constexpr char kStaleSuffix[] = " (stale)";

}  // namespace

ChecklistFeed::ChecklistFeed(plugins::ClientApi* api, plugins::FocusProvider* focus,
                             Timing timing)
    : api_(api), focus_(focus), timing_(timing) {
    // Constructor BODY, not an initializer -- see background_poller.hpp.
    Start([this] { RunLoop(); });
}

ChecklistFeed::~ChecklistFeed() {
    // Explicit, not left to ~BackgroundPoller(): destruction is derived-
    // first, so by the time the base destructor would run, the members
    // RunLoop reads are already gone. Stop() joins HERE, while they are
    // valid -- bounded by one heartbeat or one in-flight call's deadline.
    Stop();
}

void ChecklistFeed::SetFocus(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (session_id == focused_session_id_) return;
    focused_session_id_ = session_id;
    refresh_requested_ = true;
    cv_.notify_all();
}

std::string ChecklistFeed::SegmentText(const std::string& focused_session_id) const {
    SetFocus(focused_session_id);
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = cache_.find(focused_session_id);
    if (it == cache_.end()) return {};
    std::string text = FormatSegmentText(it->second.items);
    if (!text.empty() && it->second.stale) text += kStaleSuffix;
    return text;
}

ChecklistFeed::SessionSnapshot ChecklistFeed::Session(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = cache_.find(session_id);
    if (it == cache_.end()) return {};
    return SessionSnapshot{it->second.items, true, it->second.stale};
}

void ChecklistFeed::RunLoop() {
    if (api_ == nullptr) return;  // nothing can ever fill; Stop() just joins
    int stream_failures = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
        }
        bool delivered = false;
        api_->PluginSubscribe(kPluginName, kStreamMethod, "{}",
                              [this, &delivered](const std::string& payload) {
                                  delivered = true;
                                  return OnPayload(payload);
                              });
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
            // Whatever ended the stream, nothing cached is being kept
            // current any more.
            for (auto& [id, entry] : cache_) entry.stale = true;
        }
        // A stream that delivered anything was a live one: its loss starts
        // a fresh outage. One that never delivered is the same outage going
        // on, and backs off.
        stream_failures = delivered ? 1 : stream_failures + 1;
        if (stream_failures == 1) {
            minimal::common::DefaultLogger().warn(
                "checklist-view: change stream unavailable; polling instead");
        }
        auto period = timing_.fallback_period;
        for (int i = 1; i < stream_failures && period < timing_.max_fallback_period; ++i) {
            period *= 2;
        }
        PollFallback(std::min(period, timing_.max_fallback_period));
    }
}

bool ChecklistFeed::OnPayload(const std::string& json_payload) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(json_payload);
    } catch (const std::exception&) {
        // Malformed push: skip it, but still run the seed check below.
    }
    const std::string type = message.is_object() && message.contains("type") &&
                                     message.at("type").is_string()
                                 ? message.at("type").get<std::string>()
                                 : std::string();

    if (type == "checklist" && message.contains("session_id") &&
        message.at("session_id").is_string()) {
        if (auto items = ParseItems(message); items.has_value()) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return false;
            cache_[message.at("session_id").get<std::string>()] = Entry{std::move(*items), false};
        }
    }

    if (type == "heartbeat" && focus_ != nullptr) {
        // Evict closed tabs. The provider is host code: never call out under
        // our own lock.
        const auto wanted = WantedSessions();
        std::lock_guard<std::mutex> lock(mtx_);
        std::erase_if(cache_, [&wanted](const auto& item) { return !wanted.contains(item.first); });
    }

    Seed();
    RequestRedraw();   // outside mtx_: the host's seam is host code

    std::lock_guard<std::mutex> lock(mtx_);
    refresh_requested_ = false;
    return !stop_;
}

void ChecklistFeed::Seed() {
    const auto wanted = WantedSessions();
    std::set<std::string> missing;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_ || std::chrono::steady_clock::now() < next_seed_at_) return;
        for (const auto& id : wanted) {
            // Stale entries too: after a reconnect, whatever changed while
            // the stream was down is only visible by asking.
            const auto it = cache_.find(id);
            if (it == cache_.end() || it->second.stale) missing.insert(id);
        }
    }
    if (missing.empty()) return;

    auto lists = FetchLists(missing);
    std::lock_guard<std::mutex> lock(mtx_);
    if (!lists.has_value()) {
        next_seed_at_ = std::chrono::steady_clock::now() + timing_.poll_interval;
        return;
    }
    next_seed_at_ = {};
    for (auto& [id, items] : *lists) {
        // A push that landed while the call was in flight is newer than this
        // answer: keep it.
        const auto it = cache_.find(id);
        if (it != cache_.end() && !it->second.stale) continue;
        cache_[id] = Entry{std::move(items), false};
    }
}

void ChecklistFeed::PollFallback(std::chrono::milliseconds period) {
    const auto deadline = std::chrono::steady_clock::now() + period;
    int failures = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_ || std::chrono::steady_clock::now() >= deadline) return;
            refresh_requested_ = false;
        }
        const auto wanted = WantedSessions();
        if (!wanted.empty()) {
            auto lists = FetchLists(wanted);
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
            if (lists.has_value()) {
                failures = 0;
                for (auto& [id, items] : *lists) cache_[id] = Entry{std::move(items), false};
            } else {
                for (const auto& id : wanted) {
                    if (const auto it = cache_.find(id); it != cache_.end()) {
                        it->second.stale = true;
                    }
                }
                if (++failures == 1 || failures % 10 == 0) {
                    minimal::common::DefaultLogger().warn(
                        "checklist-view: list failed {} time(s)", failures);
                }
            }
        }
        RequestRedraw();   // outside mtx_

        auto wait = timing_.poll_interval;
        for (int i = 1; i < failures && wait < timing_.max_fallback_period; ++i) wait *= 2;
        wait = std::min(wait, timing_.max_fallback_period);
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + wait),
                       [this] { return stop_ || refresh_requested_; });
    }
}

std::set<std::string> ChecklistFeed::WantedSessions() const {
    std::set<std::string> wanted;
    if (focus_ != nullptr) {
        for (const auto& tab : focus_->Tabs()) {
            if (!tab.session_id.empty()) wanted.insert(tab.session_id);
        }
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (!focused_session_id_.empty()) wanted.insert(focused_session_id_);
    return wanted;
}

std::optional<std::map<std::string, std::vector<Item>>> ChecklistFeed::FetchLists(
    const std::set<std::string>& session_ids) const {
    const nlohmann::json payload{{"session_ids", session_ids}};
    const auto response = api_->PluginCall(kPluginName, "list", payload.dump());
    if (!response.has_value()) return std::nullopt;  // refused or transport
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(*response);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!body.is_object()) return std::nullopt;
    const auto checklists = body.find("checklists");
    if (checklists == body.end() || !checklists->is_array()) return std::nullopt;

    std::map<std::string, std::vector<Item>> out;
    for (const auto& id : session_ids) out[id];  // absent from the answer = no list
    for (const auto& entry : *checklists) {
        if (!entry.is_object() || !entry.contains("session_id") ||
            !entry.at("session_id").is_string()) {
            return std::nullopt;  // shape violation: trust none of it
        }
        auto items = ParseItems(entry);
        if (!items.has_value()) return std::nullopt;
        // An older server ignores the filter and answers every session:
        // keep only what was asked for.
        const auto it = out.find(entry.at("session_id").get<std::string>());
        if (it != out.end()) it->second = std::move(*items);
    }
    return out;
}

}  // namespace minimal::checklist_view
