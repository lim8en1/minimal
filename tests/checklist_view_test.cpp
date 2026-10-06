// Tests for the checklist-view client plugin's testable halves: the pure
// wire model (checklist_model.hpp), the shared ChecklistFeed's stream-fed
// cache (compiled in directly, the bundled-plugin pattern), and the panel
// view's two surfaces. The extern "C" wiring in checklist_view_plugin.cpp
// stays out, like every other bundled plugin.
//
// The feed runs a real worker thread against a scripted server; every
// assertion on its state spins with a deadline rather than sleeping a fixed
// amount.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <nlohmann/json.hpp>

#include "checklist_feed.hpp"
#include "checklist_model.hpp"
#include "checklist_panel_view.hpp"
#include "minimal/client/render/theme.hpp"

using namespace std::chrono_literals;
using minimal::checklist_view::ChecklistFeed;
using minimal::checklist_view::ChecklistPanelView;
using minimal::checklist_view::CountOf;
using minimal::checklist_view::FormatSegmentText;
using minimal::checklist_view::Item;
using minimal::checklist_view::ParseItems;
using minimal::client::ColorDepth;
using minimal::client::Theme;
namespace plugins = minimal::client::plugins;

namespace {

constexpr int kScreenWidth = 40;

// The rendered panel as plain rows -- cell by cell, so column positions are
// real (the screen's ToString interleaves escape codes).
std::vector<std::string> ToRows(const ftxui::Element& element) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kScreenWidth),
                                        ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, element);
    std::vector<std::string> rows;
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < screen.dimx(); ++x) row += screen.at(x, y);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string ToText(const ftxui::Element& element) {
    std::string text;
    for (const auto& row : ToRows(element)) text += row + "\n";
    return text;
}

// The display column `needle` starts at in its row, or -1. Byte offsets are
// not columns once a multi-byte glyph precedes the needle, so count code
// points (every glyph here is one cell wide).
int ColumnOf(const std::vector<std::string>& rows, const std::string& needle) {
    for (const auto& row : rows) {
        const auto at = row.find(needle);
        if (at == std::string::npos) continue;
        int column = 0;
        for (std::size_t i = 0; i < at; ++i) {
            if ((static_cast<unsigned char>(row[i]) & 0xC0) != 0x80) ++column;
        }
        return column;
    }
    return -1;
}

template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

nlohmann::json ItemJson(int id, const std::string& text, const std::string& status) {
    return {{"id", id}, {"text", text}, {"status", status}};
}

std::string ChecklistPush(const std::string& session_id, nlohmann::json items) {
    return nlohmann::json{{"type", "checklist"}, {"session_id", session_id}, {"items", items}}
        .dump();
}

const nlohmann::json kThree = nlohmann::json::array({ItemJson(1, "first", "completed"),
                                                     ItemJson(2, "second", "in_progress"),
                                                     ItemJson(3, "third", "pending")});

// A scripted checklist server: PluginSubscribe delivers queued pushes, and a
// heartbeat every 10 ms while idle (the server hub's contract, which is what
// lets the subscriber stop). PluginCall answers "list" from a per-session
// table, honoring the session_ids filter unless told to act like an older
// server that ignores it.
class StreamApi final : public plugins::ClientApi {
public:
    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }

    std::optional<std::string> PluginCall(const std::string& plugin, const std::string& method,
                                          const std::string& payload) override {
        std::lock_guard<std::mutex> lock(mtx_);
        ++list_calls_;
        last_plugin_ = plugin;
        last_payload_ = payload;
        if (fail_ || method != "list") return std::nullopt;
        const auto body = nlohmann::json::parse(payload);
        auto checklists = nlohmann::json::array();
        for (const auto& [id, items] : lists_) {
            bool asked = ignore_filter_ || !body.contains("session_ids");
            if (!asked) {
                for (const auto& wanted : body.at("session_ids")) asked = asked || wanted == id;
            }
            if (asked) checklists.push_back({{"session_id", id}, {"items", items}});
        }
        return nlohmann::json{{"checklists", checklists}}.dump();
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
        const std::string&, const std::string& method, const std::string&,
        const std::function<bool(const std::string&)>& on_payload) override {
        std::unique_lock<std::mutex> lock(mtx_);
        ++subscribes_;
        last_stream_method_ = method;
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
    void SetRefuse(bool value) {
        std::lock_guard<std::mutex> lock(mtx_);
        refuse_ = value;
    }
    void SetFail(bool value) {
        std::lock_guard<std::mutex> lock(mtx_);
        fail_ = value;
    }
    void SetIgnoreFilter(bool value) {
        std::lock_guard<std::mutex> lock(mtx_);
        ignore_filter_ = value;
    }
    void SetList(const std::string& session_id, nlohmann::json items) {
        std::lock_guard<std::mutex> lock(mtx_);
        lists_[session_id] = std::move(items);
    }
    int ListCalls() {
        std::lock_guard<std::mutex> lock(mtx_);
        return list_calls_;
    }
    int Subscribes() {
        std::lock_guard<std::mutex> lock(mtx_);
        return subscribes_;
    }
    std::string LastPayload() {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_payload_;
    }
    std::string LastPlugin() {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_plugin_;
    }
    std::string LastStreamMethod() {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_stream_method_;
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool end_ = false;
    bool refuse_ = false;
    bool fail_ = false;
    bool ignore_filter_ = false;
    int subscribes_ = 0;
    int list_calls_ = 0;
    std::string last_plugin_;
    std::string last_payload_;
    std::string last_stream_method_;
    std::map<std::string, nlohmann::json> lists_;
};

// The feed reads Tabs() from its worker while a test changes them: guarded.
class StubFocus final : public plugins::FocusProvider {
public:
    std::string FocusedSessionId() const override {
        std::lock_guard<std::mutex> lock(mtx_);
        return focused_;
    }
    std::vector<plugins::TabInfo> Tabs() const override {
        std::lock_guard<std::mutex> lock(mtx_);
        return tabs_;
    }
    void Set(std::string focused, std::vector<plugins::TabInfo> tabs) {
        std::lock_guard<std::mutex> lock(mtx_);
        focused_ = std::move(focused);
        tabs_ = std::move(tabs);
    }

private:
    mutable std::mutex mtx_;
    std::string focused_;
    std::vector<plugins::TabInfo> tabs_;
};

constexpr ChecklistFeed::Timing kFastTiming{20ms, 100ms, 400ms};

// The get/list wire shape for the model tests.
const char* kGetTwo = R"({"session_id":"s1","items":[
    {"id":1,"text":"first","status":"completed"},
    {"id":2,"text":"second","status":"in_progress"},
    {"id":3,"text":"third","status":"pending"}]})";

}  // namespace

// --- the pure model -------------------------------------------------------------

TEST_CASE("ParseItems decodes the get/list shape and rejects garbage", "[checklist_view]") {
    auto items = ParseItems(nlohmann::json::parse(kGetTwo));
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 3);
    CHECK((*items)[0].status == minimal::checklist_view::Status::Completed);
    CHECK((*items)[1].status == minimal::checklist_view::Status::InProgress);
    CHECK((*items)[2].status == minimal::checklist_view::Status::Pending);

    // An EMPTY list is a real answer (op=set, items: [] clears the list) --
    // distinct from malformed, which is nullopt.
    CHECK(ParseItems(nlohmann::json::parse(R"({"items":[]})")).has_value());
    CHECK(ParseItems(nlohmann::json::parse(R"({"items":[]})"))->empty());
    CHECK_FALSE(ParseItems(nlohmann::json::parse(R"("not an object")")).has_value());
    CHECK_FALSE(ParseItems(nlohmann::json::parse(R"({"no_items":true})")).has_value());
    CHECK_FALSE(
        ParseItems(nlohmann::json::parse(R"({"items":[{"status":"completed"}]})")).has_value());
}

TEST_CASE("counts and the segment text track the three statuses", "[checklist_view]") {
    const auto items = *ParseItems(nlohmann::json::parse(kGetTwo));
    const auto counts = CountOf(items);
    CHECK(counts.done == 1);
    CHECK(counts.total == 3);
    CHECK(counts.active == 1);

    CHECK(FormatSegmentText(items) == "todo 1/3 (1 active)");
    // The unfilled-segment rule: empty renders as nothing, not "0/0".
    CHECK(FormatSegmentText({}).empty());
}

// --- the shared feed -------------------------------------------------------------

TEST_CASE("pushes fill a per-session cache; a switch to a cached session renders at once",
          "[checklist_view][feed]") {
    StreamApi api;
    ChecklistFeed feed(&api, nullptr, kFastTiming);
    api.Push(ChecklistPush("s1", kThree));
    api.Push(ChecklistPush("s2", nlohmann::json::array({ItemJson(1, "other", "pending")})));

    REQUIRE(WaitFor([&] { return feed.SegmentText("s1") == "todo 1/3 (1 active)"; }));
    REQUIRE(WaitFor([&] { return feed.Session("s2").known; }));
    // The switch is served from the cache in the same call -- no fetch gap.
    CHECK(feed.SegmentText("s2") == "todo 0/1");
    CHECK(api.LastStreamMethod() == "changes");
}

TEST_CASE("an empty push clears the session: a real state, not a failure",
          "[checklist_view][feed]") {
    StreamApi api;
    ChecklistFeed feed(&api, nullptr, kFastTiming);
    api.Push(ChecklistPush("s1", kThree));
    REQUIRE(WaitFor([&] { return !feed.SegmentText("s1").empty(); }));

    api.Push(ChecklistPush("s1", nlohmann::json::array()));
    REQUIRE(WaitFor([&] { return feed.Session("s1").items.empty(); }));
    CHECK(feed.Session("s1").known);
    CHECK_FALSE(feed.Session("s1").stale);
    CHECK(feed.SegmentText("s1").empty());   // the unfilled-segment rule
}

TEST_CASE("unseen open tabs are seeded with one filtered list call",
          "[checklist_view][feed]") {
    StreamApi api;
    api.SetList("s1", kThree);
    api.SetList("closed", kThree);   // stored, but no open tab: never asked for
    StubFocus focus;
    focus.Set("s1", {{"t1", "s1", "one"}, {"t2", "s2", "two"}, {"t3", "", "sessionless"}});
    ChecklistFeed feed(&api, &focus, kFastTiming);

    REQUIRE(WaitFor([&] { return feed.Session("s1").known && feed.Session("s2").known; }));
    CHECK(feed.Session("s1").items.size() == 3);
    CHECK(feed.Session("s2").items.empty());   // no stored list: known and empty
    CHECK(api.LastPlugin() == "checklist");
    CHECK(nlohmann::json::parse(api.LastPayload()).at("session_ids") ==
          nlohmann::json::array({"s1", "s2"}));
    // Seeded once; heartbeats after that do not re-ask.
    std::this_thread::sleep_for(100ms);
    CHECK(api.ListCalls() == 1);
    CHECK_FALSE(feed.Session("closed").known);
}

TEST_CASE("a dead stream marks lists stale, polls, and resubscribes", "[checklist_view][feed]") {
    StreamApi api;
    ChecklistFeed feed(&api, nullptr, kFastTiming);
    api.Push(ChecklistPush("s1", kThree));
    REQUIRE(WaitFor([&] { return feed.SegmentText("s1") == "todo 1/3 (1 active)"; }));

    // The server goes away and its calls fail too: last-known, marked.
    api.SetFail(true);
    api.EndStream();
    REQUIRE(WaitFor([&] { return feed.SegmentText("s1") == "todo 1/3 (1 active) (stale)"; }));
    CHECK(feed.Session("s1").stale);

    // The fallback poll recovers the list (and clears the mark)...
    api.SetList("s1", nlohmann::json::array({ItemJson(1, "first", "completed")}));
    api.SetFail(false);
    CHECK(WaitFor([&] { return feed.SegmentText("s1") == "todo 1/1"; }));
    // ...and the stream is retried once the fallback period is over.
    CHECK(WaitFor([&] { return api.Subscribes() >= 2; }));
}

TEST_CASE("an older server without the stream still fills through polling",
          "[checklist_view][feed]") {
    StreamApi api;
    api.SetRefuse(true);
    api.SetIgnoreFilter(true);   // and answers every session it has
    api.SetList("s1", kThree);
    api.SetList("elsewhere", kThree);
    ChecklistFeed feed(&api, nullptr, kFastTiming);
    CHECK(WaitFor([&] { return feed.SegmentText("s1") == "todo 1/3 (1 active)"; }));
    CHECK_FALSE(feed.Session("elsewhere").known);   // filtered client-side
}

TEST_CASE("a server that keeps refusing the stream is retried with backoff",
          "[checklist_view][feed]") {
    StreamApi api;
    api.SetRefuse(true);
    ChecklistFeed feed(&api, nullptr, kFastTiming);
    // Fallback periods 100, 200, 400, 400 ms: after ~0.75 s the fixed period
    // would have subscribed 8 times, the backoff about 3.
    std::this_thread::sleep_for(750ms);
    CHECK(api.Subscribes() <= 4);
    CHECK(api.Subscribes() >= 2);
}

TEST_CASE("closed tabs are evicted on a heartbeat", "[checklist_view][feed]") {
    StreamApi api;
    StubFocus focus;
    focus.Set("s1", {{"t1", "s1", "one"}});
    ChecklistFeed feed(&api, &focus, kFastTiming);
    api.Push(ChecklistPush("s2", kThree));   // not an open tab
    api.Push(ChecklistPush("s1", kThree));
    REQUIRE(WaitFor([&] { return feed.Session("s1").known; }));
    std::this_thread::sleep_for(100ms);      // several heartbeats
    CHECK_FALSE(feed.Session("s2").known);
}

TEST_CASE("a null api degrades to a quiet feed; teardown stays bounded",
          "[checklist_view][feed]") {
    SECTION("no api at all") {
        ChecklistFeed feed(nullptr, nullptr, kFastTiming);
        CHECK(feed.SegmentText("s1").empty());
        CHECK_FALSE(feed.Session("s1").known);
    }
    SECTION("teardown is bounded by the stream's heartbeat") {
        StreamApi api;
        const auto start = std::chrono::steady_clock::now();
        {
            ChecklistFeed feed(&api, nullptr, kFastTiming);
            REQUIRE(WaitFor([&] { return api.Subscribes() == 1; }));
        }
        CHECK(std::chrono::steady_clock::now() - start < 1s);
    }
}

// --- the panel view ----------------------------------------------------------------

TEST_CASE("the panel renders the focused surface, and Tab flips to the summary",
          "[checklist_view][panel]") {
    StreamApi api;
    StubFocus focus;
    focus.Set("s1", {{"t1", "s1", "first tab"},
                     {"t2", "s2", "second tab"},
                     {"t3", "s3", "idle tab"}});
    api.SetList("s1", kThree);
    api.SetList("s2", nlohmann::json::array({ItemJson(1, "other", "in_progress")}));

    auto theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    auto feed = std::make_shared<ChecklistFeed>(&api, &focus, kFastTiming);
    ChecklistPanelView view(feed, &focus);
    view.OnTheme(theme);
    ftxui::Component component = view.component();

    // Focused surface: the current tab's items with a footer count, every
    // item's text in the same column whatever its status glyph.
    REQUIRE(WaitFor([&] { return feed->Session("s1").known && feed->Session("s3").known; }));
    const auto focused_rows = ToRows(component->Render());
    const std::string focused_text = ToText(component->Render());
    CHECK(focused_text.find("this tab") != std::string::npos);
    CHECK(focused_text.find("1/3 done") != std::string::npos);
    CHECK(ColumnOf(focused_rows, "first") == 4);
    CHECK(ColumnOf(focused_rows, "second") == 4);
    CHECK(ColumnOf(focused_rows, "third") == 4);

    // Tab flips to the summary: one line per open tab WITH tasks, display
    // names from the focus seam, the focused one marked.
    REQUIRE(component->OnEvent(ftxui::Event::Tab));
    const std::string summary_text = ToText(component->Render());
    CHECK(summary_text.find("all tabs") != std::string::npos);
    CHECK(summary_text.find("▸ first tab") != std::string::npos);
    CHECK(summary_text.find("second tab  0/1  1 active") != std::string::npos);
    CHECK(summary_text.find("idle tab") == std::string::npos);   // no tasks: not "0/0"

    // Tab flips back.
    REQUIRE(component->OnEvent(ftxui::Event::Tab));
    CHECK(ToText(component->Render()).find("this tab") != std::string::npos);
}

TEST_CASE("the panel degrades: unfetched, stale, empty and sessionless states",
          "[checklist_view][panel]") {
    StreamApi api;
    StubFocus focus;
    focus.Set("s1", {});
    api.SetFail(true);   // the seed cannot land yet

    auto theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    auto feed = std::make_shared<ChecklistFeed>(&api, &focus, kFastTiming);
    ChecklistPanelView view(feed, &focus);
    view.OnTheme(theme);
    ftxui::Component component = view.component();

    // Before anything is known: "fetching", never a blank or a crash.
    (void)feed->SegmentText("s1");   // the segment's focus hint
    CHECK(ToText(component->Render()).find("fetching") != std::string::npos);

    // A well-formed EMPTY list is "(no tasks)".
    api.Push(ChecklistPush("s1", nlohmann::json::array()));
    REQUIRE(WaitFor([&] { return feed->Session("s1").known; }));
    CHECK(ToText(component->Render()).find("no tasks") != std::string::npos);

    // A lost stream keeps what was known and says so.
    api.Push(ChecklistPush("s1", kThree));
    REQUIRE(WaitFor([&] { return feed->Session("s1").items.size() == 3; }));
    api.EndStream();
    REQUIRE(WaitFor([&] { return feed->Session("s1").stale; }));
    const std::string stale_text = ToText(component->Render());
    CHECK(stale_text.find("stale") != std::string::npos);
    CHECK(stale_text.find("first") != std::string::npos);

    // A sessionless tab says so rather than "fetching" forever.
    focus.Set("", {});
    CHECK(ToText(component->Render()).find("no session") != std::string::npos);
}
