// The schema form (client core: schema_form_model.hpp, schema_form.hpp), the
// plugin modal host's Escape handoff to it, and config-view's feed over the
// `plugins` endpoint.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

#include "config_view.hpp"
#include "ftxui_harness.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/schema_form.hpp"
#include "minimal/client/view/schema_form_model.hpp"

using namespace minimal;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// A `describe` answer: two top-level fields, a group of two, one secret.
const json kDescribe = json::parse(R"({
  "name": "sample", "file": "plugins.d/sample.json",
  "schema": {"file": "plugins.d/sample.json", "fields": [
    {"path": "enabled", "type": "boolean", "default": true, "apply": "restart",
     "doc": "Turns it on."},
    {"path": "mode", "type": "enum", "choices": ["fast", "slow", "off"], "default": "fast",
     "apply": "restart"},
    {"path": "net.timeout_ms", "type": "integer", "default": 1000, "minimum": 1,
     "apply": "live", "doc": "How long."},
    {"path": "net.api_key", "type": "string", "default": "", "secret": true,
     "apply": "restart"}
  ]},
  "config": {"mode": "slow", "net": {"api_key": "sk-literal"}},
  "pending": ["mode"]
})");

client::Theme TestTheme() {
    return client::Theme::Parse(R"({"name":"test","colors":{}})", client::ColorDepth::Truecolor);
}

}  // namespace

TEST_CASE("form rows: top-level fields first, then groups; set, default, pending, masked",
          "[schema_form]") {
    const auto rows = client::BuildFormRows(kDescribe);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].path == "enabled");
    CHECK_FALSE(rows[0].overridden);
    CHECK(rows[0].shown == "true");
    CHECK(rows[1].path == "mode");
    CHECK(rows[1].overridden);
    CHECK(rows[1].pending);
    CHECK(rows[1].shown == "slow");
    CHECK(rows[2].header);
    CHECK(rows[2].label == "net");
    CHECK(rows[3].label == "timeout_ms");
    CHECK(rows[3].live);
    CHECK(rows[4].shown == "\"****\"");  // a secret's literal never shows

    CHECK(client::ActionFor(rows[0]) == client::FormAction::kToggle);
    CHECK(client::NextValueText(rows[0]) == "false");
    CHECK(client::ActionFor(rows[1]) == client::FormAction::kCycle);
    CHECK(client::NextValueText(rows[1]) == "off");  // slow -> off
    CHECK(client::ActionFor(rows[3]) == client::FormAction::kEdit);
    CHECK(client::EditText(rows[3]) == "1000");
    CHECK(client::EditText(rows[4]).empty());  // nor is it put back in the editor

    CHECK(client::NextFieldRow(rows, 1, +1) == 3);  // the header is skipped
    CHECK(client::NextFieldRow(rows, 3, -1) == 1);
    CHECK(client::NextFieldRow(rows, 0, -1) == 0);
}

TEST_CASE("form rows carry why a field is unused, and the form says it", "[schema_form]") {
    json describe = kDescribe;
    describe["unused"] = {{"net.api_key", "mode is slow"}};
    const auto rows = client::BuildFormRows(describe);
    CHECK(rows[4].unused == "mode is slow");
    CHECK(rows[3].unused.empty());

    client::SchemaFormState state{"t", describe, "", false, 1};
    const auto theme = TestTheme();
    auto form = client::MakeSchemaForm([&] { return state; }, {}, theme);
    std::string frame = minimal_test::RenderFrame(form, 100, 24);
    CHECK(frame.find("not used") != std::string::npos);
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    frame = minimal_test::RenderFrame(form, 100, 24);
    CHECK(frame.find("Not used while mode is slow.") != std::string::npos);
}

TEST_CASE("the form: toggle, cycle, edit, reset; the editor claims Escape", "[schema_form]") {
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> unsets;
    bool back = false;
    client::SchemaFormState state{"plugins.d/sample.json", kDescribe, "", false, 1};
    const auto theme = TestTheme();
    auto form = client::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& p, const std::string& t) { sets.emplace_back(p, t); },
         .on_unset = [&](const std::string& p) { unsets.push_back(p); },
         .on_back = [&] { back = true; }},
        theme);
    const auto* aware = dynamic_cast<const client::EscapeAware*>(form.get());
    REQUIRE(aware != nullptr);

    const std::string frame = minimal_test::RenderFrame(form, 100, 24);
    CHECK(frame.find("timeout_ms") != std::string::npos);
    CHECK(frame.find("restart pending") != std::string::npos);
    CHECK(frame.find("Turns it on.") != std::string::npos);  // the first row's doc

    minimal_test::SendEvent(form, ftxui::Event::Return);  // enabled: toggle
    REQUIRE(sets.size() == 1);
    CHECK(sets[0] == std::pair<std::string, std::string>{"enabled", "false"});

    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);  // mode
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(unsets == std::vector<std::string>{"mode"});

    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);  // over the header: timeout_ms
    minimal_test::SendEvent(form, ftxui::Event::Return);     // opens the editor at "1000"
    CHECK(aware->WantsEscape());
    minimal_test::SendEvent(form, ftxui::Event::Backspace);
    minimal_test::SendEvent(form, ftxui::Event::Character('5'));
    minimal_test::SendEvent(form, ftxui::Event::Return);
    REQUIRE(sets.size() == 2);
    CHECK(sets[1] == std::pair<std::string, std::string>{"net.timeout_ms", "1005"});
    CHECK_FALSE(aware->WantsEscape());

    minimal_test::SendEvent(form, ftxui::Event::Return);  // editor again
    minimal_test::SendEvent(form, ftxui::Event::Escape);  // cancelled: nothing sent
    CHECK(sets.size() == 2);
    CHECK_FALSE(aware->WantsEscape());

    minimal_test::SendEvent(form, ftxui::Event::ArrowLeft);
    CHECK(back);
}

TEST_CASE("the plugin modal host gives Escape to an editor that wants it, else closes",
          "[schema_form][modal_host]") {
    client::SchemaFormState state{"t", kDescribe, "", false, 1};
    const auto theme = TestTheme();
    std::vector<std::pair<std::string, std::string>> sets;
    auto form = client::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& p, const std::string& t) { sets.emplace_back(p, t); },
         .on_unset = {}},
        theme);
    auto slot = std::make_shared<ftxui::Component>(form);
    bool show = true;
    int closed = 0;
    auto host = client::MakePluginModalHost(slot, show, [&] { ++closed; });

    minimal_test::SendEvent(host, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(host, ftxui::Event::ArrowDown);  // net.timeout_ms
    minimal_test::SendEvent(host, ftxui::Event::Return);     // editing
    minimal_test::SendEvent(host, ftxui::Event::Escape);     // cancels the edit
    CHECK(closed == 0);
    minimal_test::SendEvent(host, ftxui::Event::Escape);     // now closes
    CHECK(closed == 1);
    CHECK(sets.empty());
}

namespace {

// The `plugins` endpoint in miniature: answers list/describe/set, records
// what was asked.
class EndpointApi final : public client::plugins::ClientApi {
public:
    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }
    std::optional<std::string> PluginCall(const std::string& plugin, const std::string& method,
                                          const std::string& payload) override {
        std::lock_guard<std::mutex> lock(mtx_);
        calls_.push_back(plugin + " " + method + " " + payload);
        cv_.notify_all();
        if (method == "list") {
            json rows = json::array({{{"name", "sample"}, {"configurable", true}, {"enabled", true}},
                                     {{"name", "plain"}, {"configurable", false}}});
            // Every other list grows: a rebuilt menu with more rows than the
            // last frame drew.
            if (++lists_ % 2 == 0) {
                for (const char* name : {"extra-a", "extra-b", "extra-c"}) {
                    rows.push_back({{"name", name}, {"configurable", true}, {"enabled", true}});
                }
            }
            return rows.dump();
        }
        if (method == "describe") return kDescribe.dump();
        if (method == "set") return json{{"applied", "live"}, {"changed", {"net.timeout_ms"}}}.dump();
        return std::nullopt;
    }
    client::SubscribeOutcome PluginSubscribe(const std::string&, const std::string&,
                                             const std::string&,
                                             const std::function<bool(const std::string&)>&) override {
        return client::SubscribeOutcome::kError;
    }

    // Waits until `count` calls have arrived; their log.
    std::vector<std::string> WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, 5s, [&] { return calls_.size() >= count; });
        return calls_;
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::string> calls_;
    int lists_ = 0;
};

template <typename Pred>
bool Eventually(Pred pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("config-view's feed: lists configurable plugins, opens one, sets through the endpoint",
          "[schema_form][config_view]") {
    EndpointApi api;
    config_view::ConfigFeed feed(&api, nullptr);

    feed.RefreshList();
    REQUIRE(Eventually([&] { return feed.list().status.empty() && !feed.list().rows.empty(); }));
    CHECK(feed.list().rows.size() == 1);  // "plain" takes no configuration

    feed.Open("sample");
    REQUIRE(Eventually([&] { return !feed.form().describe.is_null(); }));
    CHECK(feed.form().title == "plugins.d/sample.json");

    feed.Set("net.timeout_ms", "2000");
    REQUIRE(Eventually([&] { return !feed.form().status.empty(); }));
    CHECK(feed.form().status == "net.timeout_ms saved -- applied, no restart needed");
    // list, describe (open), set, describe (the new values).
    const auto calls = api.WaitFor(4);
    REQUIRE(calls.size() == 4);
    CHECK(calls[2] == R"(plugins set {"key":"net.timeout_ms","name":"sample","text":"2000"})");

    feed.Back();
    CHECK(feed.open_plugin().empty());
}

// A user reported a segfault while moving around /config. This drives the
// whole view -- list and form, through the feed's worker -- with random keys
// and mouse events, rendering only some of the time (a terminal batches
// input: several events can land between two frames). Run it under ASan.
TEST_CASE("config-view survives random navigation, keys and mouse", "[schema_form][config_view]") {
    EndpointApi api;
    config_view::ConfigView view(&api, nullptr);
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();

    std::mt19937 rng(1234);
    const auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    const auto mouse = [&] {
        ftxui::Mouse m;
        const ftxui::Mouse::Button buttons[] = {ftxui::Mouse::Left, ftxui::Mouse::None,
                                                ftxui::Mouse::WheelUp, ftxui::Mouse::WheelDown};
        const ftxui::Mouse::Motion motions[] = {ftxui::Mouse::Pressed, ftxui::Mouse::Released,
                                                ftxui::Mouse::Moved};
        m.button = buttons[pick(4)];
        m.motion = motions[pick(3)];
        m.x = pick(100);
        m.y = pick(30);
        return ftxui::Event::Mouse("", m);
    };
    const ftxui::Event keys[] = {
        ftxui::Event::ArrowUp,   ftxui::Event::ArrowDown, ftxui::Event::ArrowDown,
        ftxui::Event::Return,    ftxui::Event::Return,    ftxui::Event::ArrowLeft,
        ftxui::Event::Backspace, ftxui::Event::Escape,    ftxui::Event::Character('d'),
        ftxui::Event::Character('s'), ftxui::Event::Character('7'), ftxui::Event::PageDown,
        ftxui::Event::Home,      ftxui::Event::End,       ftxui::Event::Tab,
    };
    for (int i = 0; i < 3000; ++i) {
        const ftxui::Event event = pick(3) == 0 ? mouse() : keys[pick(15)];
        minimal_test::SendEvent(component, event);
        if (pick(3) == 0) (void)minimal_test::RenderFrame(component, 100, 30);
        if (pick(50) == 0) std::this_thread::sleep_for(2ms);  // let answers land
    }
    SUCCEED("no crash");
}
