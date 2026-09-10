// Tests for the checklist-view client plugin's testable halves: the pure
// wire model (checklist_model.hpp), the shared ChecklistPoller's cache and
// focus semantics (compiled in directly, the bundled-plugin pattern), and
// the panel view's two surfaces. The extern "C" wiring in
// checklist_view_plugin.cpp stays out, like every other bundled plugin.
//
// The poller runs a real worker thread against stub seams; every assertion
// on its state spins on the accessor with a deadline rather than sleeping a
// fixed amount -- the fetch itself is immediate, the poll cadence is not
// part of what these tests pin down.

#include <chrono>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <nlohmann/json.hpp>

#include "checklist_model.hpp"
#include "checklist_panel_view.hpp"
#include "checklist_poller.hpp"
#include "minimal/client/render/theme.hpp"

using namespace std::chrono_literals;
using minimal::checklist_view::ChecklistPanelView;
using minimal::checklist_view::ChecklistPoller;
using minimal::checklist_view::CountOf;
using minimal::checklist_view::FormatSegmentText;
using minimal::checklist_view::Item;
using minimal::checklist_view::ParseItems;
using minimal::client::ColorDepth;
using minimal::client::Theme;
namespace plugins = minimal::client::plugins;

namespace {

std::string ToText(const ftxui::Element& element) {
    // 40x24 is enough for every fixture here; the panel wraps, it never
    // clips horizontally.
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(40),
                                        ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, element);
    return screen.ToString();
}

// Spins on a poller state change with a deadline. The worker fetches on its
// first loop iteration, so "it happened" is milliseconds; the deadline only
// bounds a regression.
template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds timeout = 3000ms) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// The get/list responses the poller should see, per method. Empty optional =
// transport failure (PluginCall's no-offer/refused answer).
class StubClientApi final : public plugins::ClientApi {
public:
    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }

    std::optional<std::string> PluginCall(const std::string& plugin, const std::string& method,
                                          const std::string& payload) override {
        std::lock_guard<std::mutex> lock(mtx_);
        ++calls;
        last_plugin = plugin;
        last_method = method;
        last_payload = payload;
        if (fail) return std::nullopt;
        if (method == "get") return get_response;
        if (method == "list") return list_response;
        return std::nullopt;
    }

    minimal::client::SubscribeOutcome PluginSubscribe(const std::string&, const std::string&,
                                              const std::string&,
                                              const std::function<bool(const std::string&)>&)
        override {
        return minimal::client::SubscribeOutcome::kError;
    }

    std::string get_response;
    std::string list_response;
    bool fail = false;

    mutable std::mutex mtx_;
    int calls = 0;
    std::string last_plugin;
    std::string last_method;
    std::string last_payload;
};

class StubFocusProvider final : public plugins::FocusProvider {
public:
    std::string FocusedSessionId() const override { return focused; }
    std::vector<plugins::TabInfo> Tabs() const override { return tabs; }

    std::string focused;
    std::vector<plugins::TabInfo> tabs;
};

// nlohmann re-serializes the items arrays; ids are optional on the wire.
const char* kGetTwo = R"({"session_id":"s1","items":[
    {"id":1,"text":"first","status":"completed"},
    {"id":2,"text":"second","status":"in_progress"},
    {"id":3,"text":"third","status":"pending"}]})";

const char* kListTwo = R"({"checklists":[
    {"session_id":"s1","items":[{"id":1,"text":"first","status":"completed"}]},
    {"session_id":"s2","items":[{"id":1,"text":"other","status":"in_progress"}]}]})";

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

// --- the shared poller -----------------------------------------------------------

TEST_CASE("the poller fetches the focused session and every stored list",
          "[checklist_view][poller]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    api.get_response = kGetTwo;
    api.list_response = kListTwo;

    ChecklistPoller poller(&api, &focus, 20ms);
    REQUIRE(WaitFor([&] { return poller.Focused().ever_fetched; }));
    CHECK(poller.Focused().valid);
    CHECK(poller.Focused().session_id == "s1");
    CHECK(poller.Focused().items.size() == 3);
    CHECK(poller.FocusedText() == "todo 1/3 (1 active)");
    CHECK(poller.AllEverFetched());
    CHECK(poller.AllItems().size() == 2);

    {
        std::lock_guard<std::mutex> lock(api.mtx_);
        CHECK(api.last_plugin == "checklist");
    }
}

TEST_CASE("a focus switch clears the focused cache and refetches the new session",
          "[checklist_view][poller]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    api.get_response = kGetTwo;
    api.list_response = kListTwo;

    ChecklistPoller poller(&api, &focus, 20ms);
    REQUIRE(WaitFor([&] { return poller.Focused().ever_fetched; }));

    // The segment's nudge: switch focus. The nudge and the provider report
    // the same truth in production (both are the frame's focused session),
    // so the stub changes both. The old session's text and items must drop
    // out SYNCHRONOUSLY (before the worker refetches), so neither surface
    // can show the previous tab's checklist.
    focus.focused = "s2";
    poller.OnFocusHint("s2");
    CHECK(poller.FocusedText().empty());
    CHECK(poller.Focused().session_id == "s2");
    CHECK(poller.Focused().items.empty());

    api.get_response = R"({"session_id":"s2","items":[{"id":1,"text":"s2 task","status":"pending"}]})";
    REQUIRE(WaitFor([&] { return poller.Focused().items.size() == 1; }));
    CHECK(poller.Focused().session_id == "s2");
    CHECK(poller.FocusedText() == "todo 0/1");
}

TEST_CASE("the provider's own focus answer drives the worker too", "[checklist_view][poller]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    api.get_response = kGetTwo;
    api.list_response = kListTwo;

    ChecklistPoller poller(&api, &focus, 20ms);
    REQUIRE(WaitFor([&] { return poller.Focused().ever_fetched; }));

    // No nudge this time: only the provider changes. The worker's per-cycle
    // cross-check must converge without the segment.
    focus.focused = "s2";
    api.get_response = R"({"session_id":"s2","items":[]})";
    REQUIRE(WaitFor([&] { return poller.Focused().session_id == "s2"; }));
    // Clearing the list server-side is a real state change, not a failure.
    REQUIRE(WaitFor([&] { return poller.Focused().ever_fetched && poller.Focused().valid; }));
    CHECK(poller.FocusedText().empty());
}

TEST_CASE("a fetch failure marks the snapshot stale without losing the cache",
          "[checklist_view][poller]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    api.get_response = kGetTwo;
    api.list_response = kListTwo;

    ChecklistPoller poller(&api, &focus, 20ms);
    REQUIRE(WaitFor([&] { return poller.Focused().ever_fetched && poller.AllEverFetched(); }));

    api.fail = true;
    CHECK(WaitFor([&] { return !poller.AllValid(); }));
    CHECK(WaitFor([&] { return !poller.Focused().valid; }));
    // What was fetched stays readable: the panel renders stale data with a
    // marker, never blanks it.
    CHECK(poller.AllItems().size() == 2);
    CHECK(poller.Focused().items.size() == 3);
}

TEST_CASE("a null api or focus degrades to a quiet poller", "[checklist_view][poller]") {
    SECTION("no api at all") {
        StubFocusProvider focus;
        focus.focused = "s1";
        ChecklistPoller poller(nullptr, &focus, 20ms);
        std::this_thread::sleep_for(20ms);  // a cycle would have fetched
        CHECK_FALSE(poller.Focused().ever_fetched);
    }
    SECTION("no focus: nothing to fetch for") {
        StubClientApi api;
        ChecklistPoller poller(&api, nullptr, 20ms);
        std::this_thread::sleep_for(20ms);
        CHECK(api.calls == 0);
    }
}

// --- the panel view ----------------------------------------------------------------

TEST_CASE("the panel renders the focused surface, and Tab flips to the summary",
          "[checklist_view][panel]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    focus.tabs = {{"t1", "s1", "first tab"}, {"t2", "s2", "second tab"}};
    api.get_response = kGetTwo;
    api.list_response = kListTwo;

    auto theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    auto poller = std::make_shared<ChecklistPoller>(&api, &focus, 20ms);
    ChecklistPanelView view(poller, &focus);
    view.OnTheme(theme);
    ftxui::Component component = view.component();

    // Focused surface: the current tab's items with a footer count.
    REQUIRE(WaitFor([&] { return poller->Focused().ever_fetched; }));
    const std::string focused_text = ToText(component->Render());
    CHECK(focused_text.find("this tab") != std::string::npos);
    CHECK(focused_text.find("first") != std::string::npos);
    CHECK(focused_text.find("1/3 done") != std::string::npos);

    // Tab flips to the summary: one line per OPEN tab, display names from
    // the focus seam, the focused one marked.
    REQUIRE(component->OnEvent(ftxui::Event::Tab));
    const std::string summary_text = ToText(component->Render());
    CHECK(summary_text.find("all tabs") != std::string::npos);
    CHECK(summary_text.find("first tab") != std::string::npos);
    CHECK(summary_text.find("second tab") != std::string::npos);
    CHECK(summary_text.find("0/1") != std::string::npos);   // s2's counts

    // Tab flips back.
    REQUIRE(component->OnEvent(ftxui::Event::Tab));
    CHECK(ToText(component->Render()).find("this tab") != std::string::npos);
}

TEST_CASE("the panel degrades: unfetched, failed, and empty states",
          "[checklist_view][panel]") {
    StubClientApi api;
    StubFocusProvider focus;
    focus.focused = "s1";
    focus.tabs = {};

    auto theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    auto poller = std::make_shared<ChecklistPoller>(&api, &focus, 20ms);
    ChecklistPanelView view(poller, &focus);
    view.OnTheme(theme);
    ftxui::Component component = view.component();

    // Before any fetch: "fetching", never a blank or a crash.
    CHECK(ToText(component->Render()).find("fetching") != std::string::npos);

    // After a transport failure: the stale marker, on both surfaces.
    api.fail = true;
    REQUIRE(WaitFor([&] { return poller->Focused().ever_fetched; }));
    CHECK(ToText(component->Render()).find("last update failed") != std::string::npos);

    // A well-formed EMPTY list is "(no tasks)", distinct from a failure.
    api.fail = false;
    api.get_response = R"({"session_id":"s1","items":[]})";
    api.list_response = R"({"checklists":[]})";
    REQUIRE(WaitFor([&] { return poller->Focused().valid && poller->Focused().items.empty(); }));
    const std::string empty_text = ToText(component->Render());
    CHECK(empty_text.find("no tasks") != std::string::npos);
    CHECK(empty_text.find("last update failed") == std::string::npos);
}
