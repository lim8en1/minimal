#include "checklist_poller.hpp"

#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace minimal::checklist_view {

ChecklistPoller::~ChecklistPoller() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    // The join can wait out one in-flight fetch, bounded by
    // SessionClient::PluginCall's 5 s deadline. Deliberate: a detached
    // worker would keep calling into the ClientApi (and the SessionClient
    // behind it) while this destructor tears the object down -- a shutdown
    // UAF is strictly worse than a bounded stall.
    if (worker_.joinable()) worker_.join();
}

void ChecklistPoller::SwitchFocusLocked(const std::string& session_id) {
    focused_snapshot_ = FocusedSnapshot{};
    focused_snapshot_.session_id = session_id;
    focused_text_.clear();
}

void ChecklistPoller::OnFocusHint(const std::string& focused_session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (focused_session_id == focused_snapshot_.session_id) return;
    SwitchFocusLocked(focused_session_id);
    refresh_requested_ = true;
    cv_.notify_all();
}

std::string ChecklistPoller::FocusedText() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return focused_text_;
}

ChecklistPoller::FocusedSnapshot ChecklistPoller::Focused() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return focused_snapshot_;
}

std::map<std::string, std::vector<Item>> ChecklistPoller::AllItems() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return all_items_;
}

bool ChecklistPoller::AllValid() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return all_valid_;
}

bool ChecklistPoller::AllEverFetched() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return all_ever_fetched_;
}

ChecklistPoller::AllSnapshot ChecklistPoller::AllSnapshotOf() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return AllSnapshot{all_items_, all_valid_, all_ever_fetched_};
}

void ChecklistPoller::RunLoop() {
    std::unique_lock<std::mutex> lock(mtx_);
    int get_failures = 0;
    int list_failures = 0;
    while (!stop_) {
        // The FocusProvider cross-check: the segment's nudge is frame-rate
        // fast but only exists while the segment renders; the provider is
        // authoritative when present (and covers a focus change the nudge
        // somehow missed). Either path lands in the same SwitchFocusLocked
        // state.
        if (focus_ != nullptr) {
            const std::string provider_focus = focus_->FocusedSessionId();
            if (provider_focus != focused_snapshot_.session_id) {
                SwitchFocusLocked(provider_focus);
            }
        }
        const FocusedSnapshot pending = focused_snapshot_;
        lock.unlock();

        if (!pending.session_id.empty() && api_ != nullptr) {
            // get(focused) and list run CONCURRENTLY, not one after the
            // other: they feed independent surfaces (the focused panel/
            // segment vs. the summary panel), so a slow list call used to
            // add its own latency on top of get's before the focused
            // surface's own result -- freshly available sooner -- could be
            // applied. list runs on its own thread; get runs here.
            std::optional<std::map<std::string, std::vector<Item>>> all;
            std::thread list_thread([&] { all = FetchList(); });
            auto items = FetchGet(pending.session_id);
            list_thread.join();
            {
                std::lock_guard<std::mutex> apply(mtx_);
                // Only apply if still the same focus -- a slow call
                // answering after the user already switched away must not
                // clobber the new tab's cache.
                if (pending.session_id == focused_snapshot_.session_id) {
                    if (items.has_value()) {
                        focused_snapshot_.items = std::move(*items);
                        focused_snapshot_.valid = true;
                        focused_snapshot_.ever_fetched = true;
                        focused_snapshot_.fetched_at = std::chrono::steady_clock::now();
                        focused_text_ = FormatSegmentText(focused_snapshot_.items);
                        get_failures = 0;
                    } else {
                        focused_snapshot_.valid = false;
                        focused_snapshot_.ever_fetched = true;
                        // Observability: every failure class (transport,
                        // plugin error, malformed payload) used to be the
                        // same silent nullopt, retried forever with nothing
                        // in any log. Log the TRANSITION, then back off --
                        // one line at the start of an outage, not one per
                        // poll.
                        if (++get_failures == 1 || get_failures % 10 == 0) {
                            minimal::common::DefaultLogger().warn(
                                "checklist-view: get failed {} time(s) for session {}",
                                get_failures, pending.session_id);
                        }
                    }
                }
            }

            // list: the summary surface, fetched concurrently above. One
            // failing must not starve the other's cache.
            {
                std::lock_guard<std::mutex> apply(mtx_);
                if (all.has_value()) {
                    all_items_ = std::move(*all);
                    all_valid_ = true;
                    all_ever_fetched_ = true;
                    list_failures = 0;
                } else if (++list_failures == 1 || list_failures % 10 == 0) {
                    minimal::common::DefaultLogger().warn(
                        "checklist-view: list failed {} time(s)", list_failures);
                    all_valid_ = false;
                }
            }
        }
        lock.lock();
        cv_.wait_for(lock, cadence_,
                     [this] { return stop_ || refresh_requested_; });
        refresh_requested_ = false;
    }
}

std::optional<std::vector<Item>> ChecklistPoller::FetchGet(const std::string& session_id) const {
    const nlohmann::json payload{{"session_id", session_id}};
    const auto response = api_->PluginCall("checklist", "get", payload.dump());
    if (!response.has_value()) return std::nullopt;  // transport-level: try next poll
    try {
        return ParseItems(nlohmann::json::parse(*response));
    } catch (const std::exception&) {
        return std::nullopt;  // a plugin-level error payload, or malformed JSON
    }
}

std::optional<std::map<std::string, std::vector<Item>>> ChecklistPoller::FetchList() const {
    const auto response = api_->PluginCall("checklist", "list", "{}");
    if (!response.has_value()) return std::nullopt;
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
    for (const auto& entry : *checklists) {
        if (!entry.is_object() || !entry.contains("session_id") ||
            !entry.at("session_id").is_string()) {
            return std::nullopt;  // shape violation: trust none of it
        }
        auto items = ParseItems(entry);
        if (!items.has_value()) return std::nullopt;
        out[entry.at("session_id").get<std::string>()] = std::move(*items);
    }
    return out;
}

}  // namespace minimal::checklist_view
