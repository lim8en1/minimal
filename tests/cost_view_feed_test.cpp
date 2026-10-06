// cost-view's TotalsFeed (compiled in directly, the bundled-plugin pattern)
// against a scripted stream: pushes fill a per-session cache, an uncached
// focus is seeded once, a dead stream degrades to stale-marked figures and
// fallback polling, and teardown stays bounded.
//
// The feed runs a real worker thread; assertions spin on Render() with a
// deadline (the checklist-view test's WaitFor) rather than sleeping.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "cost_view_format.hpp"
#include "totals_feed.hpp"

using namespace std::chrono_literals;
using minimal::client::cost_view::kStaleSuffix;
using minimal::client::cost_view::TotalsFeed;
using minimal::client::cost_view::TotalsField;
namespace plugins = minimal::client::plugins;

namespace {

template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::string TotalsPush(const std::string& session_id, double cost) {
    return nlohmann::json{{"type", "totals"},
                          {"session_id", session_id},
                          {"turns", 1},
                          {"total_cost_known", true},
                          {"total_cost", cost},
                          {"models", nlohmann::json::array({{{"input_tokens", 1'000},
                                                             {"output_tokens", 2'000}}})}}
        .dump();
}

// A scripted cost-tracker: PluginSubscribe delivers queued pushes, and a
// heartbeat every 10 ms while idle (the server hub's contract, which is what
// lets the subscriber stop). PluginCall answers "totals" from a per-session
// table; a missing session is the server's refusal (nullopt).
class StreamApi final : public plugins::ClientApi {
public:
    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }

    std::optional<std::string> PluginCall(const std::string&, const std::string&,
                                          const std::string& payload) override {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto session_id = nlohmann::json::parse(payload).value("session_id", "");
        ++calls_[session_id];
        const auto it = totals_.find(session_id);
        if (it == totals_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<minimal::v1::ListModelsResponse> ListModels(bool, std::string*) override {
        return std::nullopt;
    }
    std::optional<minimal::v1::ListAgentPresetsResponse> ListAgentPresets(std::string*) override {
        return std::nullopt;
    }
    std::optional<minimal::v1::ListSessionsResponse> ListSessions(const SessionQuery&,
                                                                  std::string*) override {
        return std::nullopt;
    }
    std::optional<std::vector<minimal::v1::SessionSummary>> ListSubagents(const std::string&,
                                                                          std::string*) override {
        return std::nullopt;
    }
    bool DeleteSession(const std::string&, std::string*) override { return false; }
    minimal::client::SubscribeOutcome PluginSubscribe(
        const std::string&, const std::string&, const std::string&,
        const std::function<bool(const std::string&)>& on_payload) override {
        std::unique_lock<std::mutex> lock(mtx_);
        ++subscribes_;
        if (refuse_) return minimal::client::SubscribeOutcome::kError;
        for (;;) {
            cv_.wait_for(lock, 10ms, [this] { return end_ || !queue_.empty(); });
            if (end_) {
                end_ = false;
                return minimal::client::SubscribeOutcome::kError;
            }
            std::string payload = R"({"type":"heartbeat"})";
            if (!queue_.empty()) {
                payload = std::move(queue_.front());
                queue_.pop_front();
            }
            lock.unlock();
            const bool keep = on_payload(payload);
            lock.lock();
            if (!keep) return minimal::client::SubscribeOutcome::kCancelled;
        }
    }

    void Push(std::string payload) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push_back(std::move(payload));
        }
        cv_.notify_all();
    }
    void EndStream() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            end_ = true;
        }
        cv_.notify_all();
    }
    void SetRefuse(bool refuse) {
        std::lock_guard<std::mutex> lock(mtx_);
        refuse_ = refuse;
    }
    void SetTotals(const std::string& session_id, std::string body) {
        std::lock_guard<std::mutex> lock(mtx_);
        totals_[session_id] = std::move(body);
    }
    int Calls(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        return calls_[session_id];
    }
    int Subscribes() {
        std::lock_guard<std::mutex> lock(mtx_);
        return subscribes_;
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool end_ = false;
    bool refuse_ = false;
    int subscribes_ = 0;
    std::map<std::string, std::string> totals_;
    std::map<std::string, int> calls_;
};

constexpr TotalsFeed::Timing kFastTiming{20ms, 100ms};

}  // namespace

TEST_CASE("pushes fill a per-session cache; a switch to a cached session renders at once",
          "[cost_view]") {
    StreamApi api;
    TotalsFeed feed(&api, kFastTiming);
    api.Push(TotalsPush("s2", 2.0));
    api.Push(TotalsPush("s1", 1.0));

    REQUIRE(WaitFor([&] { return !feed.Render("s1", TotalsField::kCost).empty(); }));
    CHECK(feed.Render("s1", TotalsField::kCost) == "$1.00 · 1k/2k tok");
    CHECK(feed.Render("s1", TotalsField::kTokenDetail) == "i 1k · o 2k");
    // The tab switch: no blank frame, no fetch to wait on.
    CHECK(feed.Render("s2", TotalsField::kCost) == "$2.00 · 1k/2k tok");
}

TEST_CASE("a turns:0 push clears the session's figures", "[cost_view]") {
    StreamApi api;
    TotalsFeed feed(&api, kFastTiming);
    api.Push(TotalsPush("s1", 1.0));
    REQUIRE(WaitFor([&] { return !feed.Render("s1", TotalsField::kCost).empty(); }));

    api.Push(R"({"type":"totals","session_id":"s1","turns":0})");
    CHECK(WaitFor([&] { return feed.Render("s1", TotalsField::kCost).empty(); }));
}

TEST_CASE("an uncached focused session is seeded with exactly one call", "[cost_view]") {
    StreamApi api;
    api.SetTotals("s3", TotalsPush("s3", 3.0));
    TotalsFeed feed(&api, kFastTiming);

    REQUIRE(WaitFor([&] { return !feed.Render("s3", TotalsField::kCost).empty(); }));
    CHECK(feed.Render("s3", TotalsField::kCost) == "$3.00 · 1k/2k tok");
    std::this_thread::sleep_for(100ms);   // ~10 heartbeats
    CHECK(api.Calls("s3") == 1);

    // A session with nothing recorded (the server refuses) is not re-asked
    // on every heartbeat either.
    CHECK(feed.Render("empty", TotalsField::kCost).empty());
    REQUIRE(WaitFor([&] { return api.Calls("empty") == 1; }));
    std::this_thread::sleep_for(100ms);
    CHECK(api.Calls("empty") == 1);
}

TEST_CASE("a dead stream marks figures stale, polls, and resubscribes", "[cost_view]") {
    StreamApi api;
    TotalsFeed feed(&api, kFastTiming);
    api.Push(TotalsPush("s1", 1.0));
    REQUIRE(WaitFor([&] { return !feed.Render("s1", TotalsField::kCost).empty(); }));

    // The server goes away; its "totals" call fails too: last-known, marked.
    api.EndStream();
    const std::string stale = std::string("$1.00 · 1k/2k tok") + kStaleSuffix;
    REQUIRE(WaitFor([&] { return feed.Render("s1", TotalsField::kCost) == stale; }));

    // The fallback poll recovers the figure (and clears the mark)...
    api.SetTotals("s1", TotalsPush("s1", 1.5));
    CHECK(WaitFor(
        [&] { return feed.Render("s1", TotalsField::kCost) == "$1.50 · 1k/2k tok"; }));
    // ...and the stream is retried once the fallback period is over.
    CHECK(WaitFor([&] { return api.Subscribes() >= 2; }));
}

TEST_CASE("a server without the stream still fills through polling", "[cost_view]") {
    StreamApi api;
    api.SetRefuse(true);
    api.SetTotals("s1", TotalsPush("s1", 1.0));
    TotalsFeed feed(&api, kFastTiming);
    CHECK(WaitFor(
        [&] { return feed.Render("s1", TotalsField::kCost) == "$1.00 · 1k/2k tok"; }));
}

TEST_CASE("a closed tab's figures are forgotten, the focused one's kept", "[cost_view]") {
    StreamApi api;
    TotalsFeed feed(&api, kFastTiming);
    api.Push(TotalsPush("s2", 2.0));
    api.Push(TotalsPush("s1", 1.0));
    REQUIRE(WaitFor([&] { return !feed.Render("s1", TotalsField::kCost).empty(); }));
    REQUIRE(WaitFor([&] { return !feed.Render("s2", TotalsField::kCost).empty(); }));
    feed.SetFocus("s1");
    feed.Forget("s2");
    feed.Forget("s1");   // focused: kept
    CHECK(feed.Render("s2", TotalsField::kCost).empty());
    feed.SetFocus("s1");
    CHECK_FALSE(feed.Render("s1", TotalsField::kCost).empty());
}

TEST_CASE("teardown is bounded by the stream's heartbeat", "[cost_view]") {
    StreamApi api;
    const auto start = std::chrono::steady_clock::now();
    {
        TotalsFeed feed(&api, kFastTiming);
        REQUIRE(WaitFor([&] { return api.Subscribes() == 1; }));
    }
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}
