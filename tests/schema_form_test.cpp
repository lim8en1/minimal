// The schema form (client core: schema_form_model.hpp, schema_form.hpp), the
// plugin modal host's Escape handoff to it, and config-view's feed over the
// `plugins` endpoint.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

#include "config_view.hpp"
#include "ftxui_harness.hpp"
#include "widget_kit_fixture.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "schema_form.hpp"
#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/sdk/schema_form_model.hpp"

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

// A form's state at generation 1, with no edit in flight.
client::SchemaFormState FormState(std::string title, json describe, std::string status = "") {
    client::SchemaFormState state;
    state.title = std::move(title);
    state.describe = std::move(describe);
    state.status = std::move(status);
    state.generation = 1;
    return state;
}

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

    // With a chooser on offer, an enum and a formatted field are handed to it.
    CHECK(client::ActionFor(rows[1], /*can_pick=*/true) == client::FormAction::kPick);
    CHECK(client::ActionFor(rows[0], /*can_pick=*/true) == client::FormAction::kToggle);
    CHECK(client::ActionFor(rows[3], /*can_pick=*/true) == client::FormAction::kEdit);
    client::FormRow model = rows[3];
    model.type = "string";
    model.field["format"] = "model_ref";
    CHECK(client::ActionFor(model, /*can_pick=*/true) == client::FormAction::kPick);
    CHECK(client::ActionFor(model) == client::FormAction::kEdit);

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

    client::SchemaFormState state = FormState("t", describe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm([&] { return state; }, {}, theme);
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
    client::SchemaFormState state = FormState("plugins.d/sample.json", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
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

    minimal_test::SendEvent(form, ftxui::Event::Return);  // editor again
    minimal_test::SendEvent(form, ftxui::Event::Escape);  // cancelled: nothing sent
    CHECK(sets.size() == 2);
    CHECK_FALSE(back);   // the editor's Escape only closed the editor

    minimal_test::SendEvent(form, ftxui::Event::ArrowLeft);
    CHECK(back);
}

TEST_CASE("Enter on an untouched secret does not erase it", "[schema_form][regression]") {
    // The bug: the editor starts a secret empty (its literal is never put
    // back on screen), so Enter without typing saved "" over the stored key.
    const auto rows = client::BuildFormRows(kDescribe);
    CHECK_FALSE(client::SubmitSaves(rows[4], ""));
    CHECK(client::SubmitSaves(rows[4], "sk-new"));
    CHECK(client::SubmitSaves(rows[3], ""));  // a non-secret's empty text is a value

    std::vector<std::pair<std::string, std::string>> sets;
    client::SchemaFormState state = FormState("plugins.d/sample.json", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& p, const std::string& t) { sets.emplace_back(p, t); }},
        theme);
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::Return);  // net.api_key: the editor, empty
    minimal_test::SendEvent(form, ftxui::Event::Return);  // nothing typed
    CHECK(sets.empty());
    const auto* aware = dynamic_cast<const client::EscapeAware*>(form.get());
    REQUIRE(aware != nullptr);
    CHECK_FALSE(aware->WantsEscape());  // the editor closed all the same
}

TEST_CASE("the selected field's details and the status stay on screen below a long list",
          "[schema_form][regression]") {
    // The bug: details and status were rows of the scrolling body, below
    // every field -- on a long form the outcome of an edit near the top was
    // scrolled out of sight.
    json describe = kDescribe;
    for (int i = 0; i < 40; ++i) {
        describe["schema"]["fields"].push_back(
            {{"path", "extra_" + std::to_string(i)}, {"type", "integer"}, {"default", i},
             {"apply", "live"}});
    }
    client::SchemaFormState state = FormState("t", describe, "enabled saved -- the outcome");
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm([&] { return state; }, {}, theme);
    const std::string frame = minimal_test::RenderFrame(form, 100, 24);
    CHECK(frame.find("enabled saved -- the outcome") != std::string::npos);
    CHECK(frame.find("Turns it on.") != std::string::npos);
    CHECK(frame.find("esc: close") != std::string::npos);  // the hints too
}

TEST_CASE("while an edit is in flight the form takes no edit keys", "[schema_form][regression]") {
    // The bug: a toggle computes its value from the last answer; two quick
    // presses before the refetch sent the same value twice -- the second
    // press did nothing.
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> unsets;
    client::SchemaFormState state = FormState("t", kDescribe);
    state.busy = true;
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& p, const std::string& t) { sets.emplace_back(p, t); },
         .on_unset = [&](const std::string& p) { unsets.push_back(p); }},
        theme);
    minimal_test::SendEvent(form, ftxui::Event::Return);
    minimal_test::SendEvent(form, ftxui::Event::Character(' '));
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(sets.empty());
    CHECK(unsets.empty());
    CHECK(minimal_test::RenderFrame(form, 100, 24).find("saving...") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);  // moving is fine
    minimal_test::SendEvent(form, ftxui::Event::ArrowUp);

    state.busy = false;
    minimal_test::SendEvent(form, ftxui::Event::Return);
    CHECK(sets.size() == 1);
}

TEST_CASE("a secret is typed masked", "[schema_form][regression]") {
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm([&] { return state; }, {}, theme);
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::Return);  // net.api_key: the editor
    for (const char c : std::string("sk-typed")) {
        minimal_test::SendEvent(form, ftxui::Event::Character(c));
    }
    const std::string frame = minimal_test::RenderFrame(form, 100, 24);
    CHECK(frame.find("sk-typed") == std::string::npos);
    CHECK(frame.find("empty keeps the current key") != std::string::npos);

    // A plain field's editor shows its text.
    minimal_test::SendEvent(form, ftxui::Event::Escape);
    minimal_test::SendEvent(form, ftxui::Event::ArrowUp);  // net.timeout_ms
    minimal_test::SendEvent(form, ftxui::Event::Return);
    minimal_test::SendEvent(form, ftxui::Event::Character('7'));
    CHECK(minimal_test::RenderFrame(form, 100, 24).find("10007") != std::string::npos);
}

TEST_CASE("resetting a set secret takes a second d", "[schema_form][regression]") {
    // The bug: one keystroke erased the configured key.
    std::vector<std::string> unsets;
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; }, {.on_unset = [&](const std::string& p) { unsets.push_back(p); }},
        theme);
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(unsets.empty());
    CHECK(minimal_test::RenderFrame(form, 100, 24).find("d again") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::ArrowUp);    // any other key calls it off
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(unsets.empty());
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(unsets == std::vector<std::string>{"net.api_key"});

    minimal_test::SendEvent(form, ftxui::Event::ArrowUp);  // a plain field: one d
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(unsets.size() == 2);
}

TEST_CASE("a refused edit reopens the editor with what was typed", "[schema_form][regression]") {
    // The bug: the editor closed on Enter, before the answer; a refusal lost
    // the text.
    std::vector<std::pair<std::string, std::string>> sets;
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& p, const std::string& t) { sets.emplace_back(p, t); }},
        theme);
    const auto* aware = dynamic_cast<const client::EscapeAware*>(form.get());
    REQUIRE(aware != nullptr);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);  // net.timeout_ms
    minimal_test::SendEvent(form, ftxui::Event::Return);
    minimal_test::SendEvent(form, ftxui::Event::Character('x'));
    minimal_test::SendEvent(form, ftxui::Event::Return);
    REQUIRE(sets.size() == 1);
    CHECK_FALSE(aware->WantsEscape());

    state.status = "net.timeout_ms: not an integer";
    state.status_is_error = true;
    state.error_path = "net.timeout_ms";
    ++state.generation;
    (void)minimal_test::RenderFrame(form, 100, 24);
    CHECK(aware->WantsEscape());
    minimal_test::SendEvent(form, ftxui::Event::Backspace);
    minimal_test::SendEvent(form, ftxui::Event::Return);
    REQUIRE(sets.size() == 2);
    CHECK(sets[1] == std::pair<std::string, std::string>{"net.timeout_ms", "1000"});

    // Accepted: the editor stays closed.
    state.status = "net.timeout_ms saved";
    state.status_is_error = false;
    state.error_path.clear();
    ++state.generation;
    (void)minimal_test::RenderFrame(form, 100, 24);
    CHECK_FALSE(aware->WantsEscape());
}

TEST_CASE("the plugin modal host gives Escape to an editor that wants it, else closes",
          "[schema_form][modal_host]") {
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    std::vector<std::pair<std::string, std::string>> sets;
    auto form = client::ui_kit::MakeSchemaForm(
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
        if (method == "set") {
            while (hold_) std::this_thread::sleep_for(1ms);  // an answer still on its way
        }
        std::lock_guard<std::mutex> lock(mtx_);
        calls_.push_back(plugin + " " + method + " " + payload);
        cv_.notify_all();
        if (down_) return std::nullopt;  // the server restarting: no answer
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
        if (method == "describe") return describe_.dump();
        if (method == "enable") {
            const json request = json::parse(payload);
            if (request.value("name", "") == "provider-only") {
                return json{{"error", "disabling provider-only would leave no provider"}}.dump();
            }
            return json{{"applied", "restart"}, {"enabled", request.value("enabled", true)}}.dump();
        }
        if (method == "set" && refuse_) return json{{"error", "limit: must be at least 1"}}.dump();
        if (method == "set") return json{{"applied", "live"}, {"changed", {"net.timeout_ms"}}}.dump();
        return std::nullopt;
    }
    std::optional<minimal::v1::ListModelsResponse> ListModels(bool, std::string*) override {
        minimal::v1::ListModelsResponse response;
        for (const char* ref : {"main/alpha", "main/beta"}) response.add_models()->set_ref(ref);
        return response;
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

public:
    std::atomic<bool> down_{false};
    std::atomic<bool> hold_{false};    // `set` waits until cleared
    json describe_ = kDescribe;        // what `describe` answers; set before use
    std::atomic<bool> refuse_{false};  // `set` answers an error
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

TEST_CASE("config-view's feed: lists every plugin, opens one, sets through the endpoint",
          "[schema_form][config_view]") {
    EndpointApi api;
    config_view::ConfigFeed feed(&api, nullptr);

    feed.RefreshList();
    REQUIRE(Eventually([&] { return feed.list().status.empty() && !feed.list().rows.empty(); }));
    CHECK(feed.list().rows.size() == 2);  // "plain" takes no configuration, but can be toggled

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

TEST_CASE("config-view's feed: a toggle goes through `enable`, its outcome or refusal kept",
          "[schema_form][config_view][toggle]") {
    EndpointApi api;
    config_view::ConfigFeed feed(&api, nullptr);
    feed.Toggle("plain", false);
    REQUIRE(Eventually([&] { return !feed.list().outcome.empty(); }));
    CHECK(feed.list().outcome == "plain off at the next server start");
    feed.Toggle("provider-only", false);
    REQUIRE(Eventually([&] { return feed.list().outcome.find("no provider") != std::string::npos; }));
    const auto calls = api.WaitFor(4);
    CHECK(calls[0] == R"(plugins enable {"enabled":false,"name":"plain"})");
    CHECK(calls[1].rfind("plugins list", 0) == 0);   // the list refreshed after it
}

TEST_CASE("config-view's feed: busy until an edit's answer lands; a refusal names its field",
          "[schema_form][config_view]") {
    EndpointApi api;
    config_view::ConfigFeed feed(&api, nullptr);
    feed.Open("sample");
    REQUIRE(Eventually([&] { return !feed.form().describe.is_null(); }));

    api.hold_ = true;
    feed.Set("net.timeout_ms", "2000");
    CHECK(feed.form().busy);
    api.hold_ = false;
    REQUIRE(Eventually([&] { return !feed.form().busy; }));
    CHECK(feed.form().error_path.empty());
    CHECK_FALSE(feed.form().status_is_error);

    api.refuse_ = true;
    feed.Set("net.timeout_ms", "0");
    REQUIRE(Eventually([&] { return feed.form().status_is_error; }));
    CHECK(feed.form().error_path == "net.timeout_ms");
    CHECK(feed.form().status == "limit: must be at least 1");
    CHECK_FALSE(feed.form().busy);
}

TEST_CASE("config-view drops what it showed when a refetch fails", "[schema_form][config_view]") {
    // Regression: across a server restart the first fetch can fail, and the
    // feed kept the old server's rows ("6 set · restart pending") beside the
    // error -- changes the new server had already applied read as pending.
    EndpointApi api;
    config_view::ConfigFeed feed(&api, nullptr);
    feed.RefreshList();
    REQUIRE(Eventually([&] { return !feed.list().rows.empty(); }));
    feed.Open("sample");
    REQUIRE(Eventually([&] { return !feed.form().describe.is_null(); }));

    api.down_ = true;
    feed.Open("sample");
    REQUIRE(Eventually([&] { return feed.form().status_is_error; }));
    CHECK(feed.form().describe.is_null());
    feed.Back();
    REQUIRE(Eventually([&] { return !feed.list().status.empty() && feed.list().status != "loading"; }));
    CHECK(feed.list().rows.empty());
}

TEST_CASE("config-view: an enum and a model ref are chosen from a list, not typed or stepped",
          "[schema_form][config_view]") {
    EndpointApi api;
    api.describe_["schema"]["fields"].push_back(
        {{"path", "model"}, {"type", "string"}, {"default", ""}, {"apply", "live"},
         {"format", "model_ref"}, {"empty_label", "the session's own model"}});
    config_view::ConfigView view(&api, &minimal_test::CoreWidgetKit(), nullptr);
    view.OnTheme(TestTheme());
    auto component = view.component();
    const auto* aware = dynamic_cast<const client::EscapeAware*>(component.get());
    REQUIRE(aware != nullptr);
    view.feed().Open("sample");
    REQUIRE(Eventually([&] { return !view.feed().form().describe.is_null(); }));
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 30); };
    (void)frame();

    // mode (an enum): its choices, the current one under the cursor.
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    std::string shown = frame();
    CHECK(shown.find("slow  (current)") != std::string::npos);
    CHECK(shown.find("fast  (default)") != std::string::npos);
    CHECK(aware->WantsEscape());  // Escape goes back to the form, not out
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // off
    minimal_test::SendEvent(component, ftxui::Event::Return);
    auto calls = api.WaitFor(3);  // describe, set, describe
    REQUIRE(calls.size() >= 2);
    CHECK(calls[1] == R"(plugins set {"key":"mode","name":"sample","text":"off"})");
    REQUIRE(Eventually([&] { return !view.feed().form().busy; }));
    CHECK(frame().find("timeout_ms") != std::string::npos);  // back on the form
    CHECK(aware->WantsEscape());   // on the form, Escape steps back to the list

    // model (a model ref; top-level, so right after mode): the server's
    // models, and "empty" to unset.
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    REQUIRE(Eventually([&] { return frame().find("alpha") != std::string::npos; }));
    CHECK(frame().find("empty -- the session's own model") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // onto the "main" header...
    (void)frame();                                                 // ...a frame walks past it
    minimal_test::SendEvent(component, ftxui::Event::Return);      // main/alpha
    calls = api.WaitFor(5);
    REQUIRE(calls.size() >= 4);
    CHECK(calls[3] == R"(plugins set {"key":"model","name":"sample","text":"main/alpha"})");
    REQUIRE(Eventually([&] { return !view.feed().form().busy; }));

    minimal_test::SendEvent(component, ftxui::Event::Return);  // the chooser again
    REQUIRE(Eventually([&] { return frame().find("alpha") != std::string::npos; }));
    minimal_test::SendEvent(component, ftxui::Event::Return);  // the "empty" row: unset
    calls = api.WaitFor(7);
    REQUIRE(calls.size() >= 6);
    CHECK(calls[5] == R"(plugins unset {"key":"model","name":"sample"})");

    // Escape in a chooser returns to the form.
    REQUIRE(Eventually([&] { return !view.feed().form().busy; }));
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(aware->WantsEscape());
    minimal_test::SendEvent(component, ftxui::Event::Escape);
    CHECK(frame().find("timeout_ms") != std::string::npos);   // the chooser closed
    minimal_test::SendEvent(component, ftxui::Event::Escape);   // the form steps back
    CHECK(view.feed().open_plugin().empty());
    CHECK_FALSE(aware->WantsEscape());   // the list: Escape closes the view
}

// A user reported a segfault while moving around /config. This drives the
// whole view -- list and form, through the feed's worker -- with random keys
// and mouse events, rendering only some of the time (a terminal batches
// input: several events can land between two frames). Run it under ASan.
TEST_CASE("config-view survives random navigation, keys and mouse", "[schema_form][config_view]") {
    EndpointApi api;
    config_view::ConfigView view(&api, &minimal_test::CoreWidgetKit(), nullptr);
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

// --- config-view polish (CONFIG_VIEW_PLAN.md F1-F7, §1.3) ----------------------

TEST_CASE("the form: Backspace stays, Escape and left step back", "[schema_form][polish]") {
    // F1/F2: Backspace is a habitual delete, often pressed right after the
    // editor closed; it used to leave the form. Escape steps back one level.
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    int backs = 0;
    auto form = client::ui_kit::MakeSchemaForm([&] { return state; },
                                               {.on_back = [&] { ++backs; }}, theme);
    const auto* aware = dynamic_cast<const client::EscapeAware*>(form.get());
    REQUIRE(aware != nullptr);
    (void)minimal_test::RenderFrame(form, 100, 24);
    minimal_test::SendEvent(form, ftxui::Event::Backspace);
    CHECK(backs == 0);
    CHECK(aware->WantsEscape());   // there is a level to step back to
    minimal_test::SendEvent(form, ftxui::Event::Escape);
    CHECK(backs == 1);
    minimal_test::SendEvent(form, ftxui::Event::ArrowLeft);
    CHECK(backs == 2);
    // Without a level to go back to, Escape is the host's (closes the view).
    auto bare = client::ui_kit::MakeSchemaForm([&] { return state; }, {}, theme);
    CHECK_FALSE(dynamic_cast<const client::EscapeAware*>(bare.get())->WantsEscape());
}

TEST_CASE("the form: only live fields are tagged; the hints follow the row",
          "[schema_form][polish]") {
    // F3, F7.
    client::SchemaFormState state = FormState("t", kDescribe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; }, {.on_set = [](const std::string&, const std::string&) {},
                                .on_back = [] {}},
        theme);
    std::string frame = minimal_test::RenderFrame(form, 110, 24);
    CHECK(frame.find("live") != std::string::npos);
    CHECK(frame.find("  restart\n") == std::string::npos);
    CHECK(frame.find("restart ") == frame.find("restart pending"));   // only the pending mark
    CHECK(frame.find("enter/space: toggle") != std::string::npos);   // enabled: a boolean
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);          // mode: set, an enum
    frame = minimal_test::RenderFrame(form, 110, 24);
    CHECK(frame.find("enter/space: next value") != std::string::npos);
    CHECK(frame.find("d/del: reset to default") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);          // net.timeout_ms
    frame = minimal_test::RenderFrame(form, 110, 24);
    CHECK(frame.find("enter: edit") != std::string::npos);
    CHECK(frame.find("d/del") == std::string::npos);                 // at its default
}

TEST_CASE("the form: a cut value is shown whole below; widths count columns",
          "[schema_form][polish]") {
    // F4 and §1.3.
    json describe = kDescribe;
    describe["schema"]["fields"].push_back({{"path", "net.endpoint_url_that_is_long"},
                                            {"type", "string"},
                                            {"default", ""},
                                            {"apply", "live"}});
    const std::string url = "https://example.invalid/a/very/long/path/that/does/not/fit";
    describe["config"]["net"]["endpoint_url_that_is_long"] = url;
    describe["schema"]["fields"].push_back(
        {{"path", "net.名前名前名前名前名前名前名前名前"}, {"type", "string"}, {"default", "x"},
         {"apply", "live"}});
    client::SchemaFormState state = FormState("t", describe);
    const auto theme = TestTheme();
    auto form = client::ui_kit::MakeSchemaForm([&] { return state; }, {}, theme);
    for (int i = 0; i < 4; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    const std::string frame = minimal_test::RenderFrame(form, 120, 30);
    CHECK(frame.find("= " + url) != std::string::npos);
    // The wide label is cut to the column (minus its indent), not past it:
    // its value starts at the same screen column as every other row's.
    const auto column_after = [&](const std::string& row_needle, const std::string& value) {
        const auto at = frame.find(row_needle);
        const auto line_start = frame.rfind('\n', at) + 1;
        const auto value_at = frame.find(value, at + row_needle.size());
        return client::Utf8ColumnWidth(frame.substr(line_start, value_at - line_start));
    };
    INFO(frame);
    CHECK(column_after("timeout_ms", "1000") == column_after("名前", "x"));
}

TEST_CASE("config-view: filters ignore case; reopening keeps the form and the selection",
          "[schema_form][config_view][polish]") {
    // F5, F6.
    EndpointApi api;
    config_view::ConfigView view(&api, &minimal_test::CoreWidgetKit(), nullptr);
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.feed().RefreshList();
    REQUIRE(Eventually([&] { return !view.feed().list().rows.empty(); }));
    (void)minimal_test::RenderFrame(component, 100, 30);
    for (const char c : std::string("SAMP")) {
        minimal_test::SendEvent(component, ftxui::Event::Character(std::string(1, c)));
    }
    CHECK(minimal_test::RenderFrame(component, 100, 30).find("sample") != std::string::npos);

    view.feed().Open("sample");
    REQUIRE(Eventually([&] { return !view.feed().form().describe.is_null(); }));
    (void)minimal_test::RenderFrame(component, 100, 30);
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);   // mode
    view.OnShow();                                                 // the view shown again
    CHECK_FALSE(view.feed().form().describe.is_null());            // no "loading" flash
    const auto generation = view.feed().form().generation;
    REQUIRE(Eventually([&] { return view.feed().form().generation > generation; }));
    const std::string frame = minimal_test::RenderFrame(component, 100, 30);
    CHECK(frame.find("enter: choose") != std::string::npos);   // still on mode (an enum)
}

namespace {

// A plugin whose config holds a list of objects (cost-tracker's rate_sources).
const json kListDescribe = json::parse(R"({
  "name": "lists", "file": "plugins.d/lists.json",
  "schema": {"file": "plugins.d/lists.json", "fields": [
    {"path": "sources", "type": "object_list", "apply": "restart",
     "default": [{"format": "openrouter", "url": "https://or"}],
     "item_template": {"format": "litellm-proxy", "url": "https://"},
     "items": [
       {"path": "format", "type": "enum", "choices": ["litellm", "openrouter", "litellm-proxy"]},
       {"path": "url", "type": "string", "required": true},
       {"path": "api_key", "type": "string", "default": "", "secret": true}
     ]},
    {"path": "timeout_ms", "type": "integer", "default": 10}
  ]},
  "config": {"sources": [{"format": "litellm", "url": "https://a", "api_key": "sk-live"},
                        {"format": "openrouter", "url": "https://b"}]}
})");

void Type(ftxui::Component& form, const std::string& text) {
    for (const char c : text) minimal_test::SendEvent(form, ftxui::Event::Character(c));
}

}  // namespace

TEST_CASE("form rows: an object list shows a row per item, masked, and an add row",
          "[schema_form][object_list]") {
    const auto rows = client::BuildFormRows(kListDescribe);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].path == "sources");
    CHECK(rows[0].shown == "2 entries");
    CHECK(rows[1].item == 0);
    CHECK(rows[1].shown == "litellm  https://a  \"****\"");   // a literal key never shows
    CHECK(rows[2].item == 1);
    CHECK(rows[2].shown == "openrouter  https://b");
    CHECK(rows[3].add_item);
    CHECK(rows[4].path == "timeout_ms");
    CHECK(client::ActionFor(rows[1]) == client::FormAction::kOpen);
    CHECK(client::ActionFor(rows[3]) == client::FormAction::kOpen);

    CHECK(client::ItemDraft(rows[3]) == json({{"format", "litellm-proxy"}, {"url", "https://"}}));
    CHECK(client::ListMoved(client::ListOf(rows[1]), 1, -1)[0]["url"] == "https://b");
    CHECK(client::ListWithout(client::ListOf(rows[1]), 0).size() == 1);

    json item = client::ItemDraft(rows[1]);
    const auto item_rows = client::BuildFormRows(client::ItemDescribe(rows[0], item));
    REQUIRE(item_rows.size() == 3);
    std::string error;
    CHECK_FALSE(client::SetItemField(item, item_rows[0], "nonsense", error));
    CHECK(error.find("must be one of") != std::string::npos);
    CHECK(client::SetItemField(item, item_rows[1], "https://c", error));
    CHECK(item["url"] == "https://c");
}

TEST_CASE("schema form: an item opens as its own form and saves the whole list on leaving",
          "[schema_form][object_list]") {
    const auto theme = TestTheme();
    auto state = FormState("plugins.d/lists.json", kListDescribe);
    std::vector<std::pair<std::string, std::string>> sets;
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& path, const std::string& text) {
             sets.emplace_back(path, text);
             state.busy = true;
         }},
        theme);
    (void)minimal_test::RenderFrame(form, 100, 30);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // item #1
    minimal_test::SendEvent(form, ftxui::Event::Return);      // open it
    std::string frame = minimal_test::RenderFrame(form, 100, 30);
    CHECK(frame.find("sources › #1") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // url
    minimal_test::SendEvent(form, ftxui::Event::Return);      // edit
    for (int i = 0; i < 9; ++i) minimal_test::SendEvent(form, ftxui::Event::Backspace);
    Type(form, "https://z");
    minimal_test::SendEvent(form, ftxui::Event::Return);      // into the draft only
    CHECK(sets.empty());
    minimal_test::SendEvent(form, ftxui::Event::Escape);      // save and back
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].first == "sources");
    const json sent = json::parse(sets[0].second);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0]["url"] == "https://z");
    CHECK(sent[0]["api_key"] == "sk-live");   // untouched fields go back as they were
    CHECK(sent[1] == kListDescribe["config"]["sources"][1]);

    // Refused: the item stays open with the plugin's reason.
    state.busy = false;
    state.generation = 2;
    state.status = "sources[0].url: unreachable";
    state.status_is_error = true;
    frame = minimal_test::RenderFrame(form, 100, 30);
    CHECK(frame.find("sources › #1") != std::string::npos);
    CHECK(frame.find("unreachable") != std::string::npos);

    // Accepted: back on the list, the item selected.
    minimal_test::SendEvent(form, ftxui::Event::Escape);
    REQUIRE(sets.size() == 2);
    state.busy = false;
    state.generation = 3;
    state.status = "saved";
    state.status_is_error = false;
    frame = minimal_test::RenderFrame(form, 100, 30);
    CHECK(frame.find("sources › #1") == std::string::npos);   // the item form's title
    CHECK(frame.find("2 entries") != std::string::npos);
}

TEST_CASE("schema form: items are added, deleted on a second d, and moved",
          "[schema_form][object_list]") {
    const auto theme = TestTheme();
    auto state = FormState("plugins.d/lists.json", kListDescribe);
    std::vector<json> sent;
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string&, const std::string& text) {
             sent.push_back(json::parse(text));
         }},
        theme);
    (void)minimal_test::RenderFrame(form, 100, 30);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // #1
    minimal_test::SendEvent(form, ftxui::Event::Character('J'));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0][0]["url"] == "https://b");
    CHECK(sent[0][1]["url"] == "https://a");

    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    CHECK(sent.size() == 1);   // armed, not done
    CHECK(minimal_test::RenderFrame(form, 100, 30).find("d again to delete") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].size() == 1);

    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // + add
    minimal_test::SendEvent(form, ftxui::Event::Return);
    CHECK(minimal_test::RenderFrame(form, 100, 30).find("(new)") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::Escape);      // a new item saves as is
    REQUIRE(sent.size() == 3);
    CHECK(sent[2].size() == 3);
    CHECK(sent[2][2] == json({{"format", "litellm-proxy"}, {"url", "https://"}}));
}

namespace {

// A plugin with a map of priced models (cost-tracker's rates) and a rubric
// with no schema (decision-advisor's questions).
const json kStructuredDescribe = json::parse(R"({
  "name": "structured", "file": "plugins.d/structured.json",
  "schema": {"file": "plugins.d/structured.json", "fields": [
    {"path": "rates", "type": "object_map", "apply": "live", "default": {},
     "key": {"doc": "the model id", "format": "model_id"},
     "item_template": {"input": 0, "output": 0},
     "items": [{"path": "input", "type": "number"}, {"path": "output", "type": "number"},
               {"path": "tier", "type": "enum", "choices": ["low", "high"]}]},
    {"path": "questions", "type": "json"}
  ]},
  "config": {
    "rates": {"gpt-4.1": {"input": 2, "output": 8}},
    "questions": {"action": {"type": "choice", "criteria": {"allow": "safe", "deny": "harm"}},
                  "tags": ["a", "b"]}
  }
})");

struct Picks {
    std::vector<client::PickRequest> requests;
};

}  // namespace

TEST_CASE("form rows: a map shows an entry per name; its form names it, renamed or not twice",
          "[schema_form][object_map]") {
    const auto rows = client::BuildFormRows(kStructuredDescribe);
    REQUIRE(rows.size() >= 4);
    CHECK(rows[0].shown == "1 entry");
    CHECK(rows[1].item_key == "gpt-4.1");   // a dotted name is a name, not a group
    CHECK(rows[1].label == "gpt-4.1");
    CHECK(rows[1].shown == "2  8");
    CHECK(rows[2].add_item);
    CHECK(client::ActionFor(rows[1]) == client::FormAction::kOpen);
    CHECK(client::ActionFor(rows[3], true) == client::FormAction::kOpen);   // questions: inferred

    json draft = client::ItemDraft(rows[1]);
    CHECK(draft[client::kMapKeyField] == "gpt-4.1");
    const auto entry_rows = client::BuildFormRows(client::ItemDescribe(rows[0], draft));
    REQUIRE(entry_rows.size() == 4);
    CHECK(entry_rows[0].label == "name");
    CHECK(client::ActionFor(entry_rows[0], /*can_pick=*/true) == client::FormAction::kPick);

    json map = rows[0].value;
    std::string error;
    draft[client::kMapKeyField] = "gpt-5";
    CHECK(client::MapWithEntry(map, "gpt-4.1", draft, error));
    CHECK(map.contains("gpt-5"));
    CHECK_FALSE(map.contains("gpt-4.1"));
    CHECK(map["gpt-5"] == json({{"input", 2}, {"output", 8}}));
    json taken = client::ItemDraft(rows[2]);   // a new entry
    taken[client::kMapKeyField] = "gpt-5";
    CHECK_FALSE(client::MapWithEntry(map, "", taken, error));
    CHECK(error.find("already") != std::string::npos);
    taken[client::kMapKeyField] = "";
    CHECK_FALSE(client::MapWithEntry(map, "", taken, error));
}

TEST_CASE("schema form: a map entry's name is chosen into the draft; the map goes out once",
          "[schema_form][object_map]") {
    const auto theme = TestTheme();
    auto state = FormState("plugins.d/structured.json", kStructuredDescribe);
    std::vector<std::pair<std::string, json>> sets;
    Picks picks;
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& path, const std::string& text) {
             sets.emplace_back(path, json::parse(text));
             state.busy = true;
         },
         .on_pick = [&](const client::PickRequest& request) { picks.requests.push_back(request); }},
        theme);
    (void)minimal_test::RenderFrame(form, 110, 30);
    minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // gpt-4.1
    minimal_test::SendEvent(form, ftxui::Event::Return);      // its form
    CHECK(minimal_test::RenderFrame(form, 110, 30).find("rates › gpt-4.1") != std::string::npos);
    minimal_test::SendEvent(form, ftxui::Event::Return);      // name: the chooser
    REQUIRE(picks.requests.size() == 1);
    CHECK(picks.requests[0].field.value("format", "") == "model_id");
    CHECK(picks.requests[0].current == "gpt-4.1");
    picks.requests[0].deliver("gpt-4.1-mini");                // chosen: into the draft
    CHECK(sets.empty());
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // tier
    minimal_test::SendEvent(form, ftxui::Event::Character(' '));   // space: next value
    minimal_test::SendEvent(form, ftxui::Event::Escape);      // keep it: the map goes out
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].first == "rates");
    CHECK(sets[0].second == json({{"gpt-4.1-mini", {{"input", 2}, {"output", 8}, {"tier", "low"}}}}));
}

TEST_CASE("schema form: a schema-less value opens as forms, level by level, and goes out once",
          "[schema_form][inferred]") {
    const auto theme = TestTheme();
    auto state = FormState("plugins.d/structured.json", kStructuredDescribe);
    std::vector<std::pair<std::string, json>> sets;
    auto form = client::ui_kit::MakeSchemaForm(
        [&] { return state; },
        {.on_set = [&](const std::string& path, const std::string& text) {
             sets.emplace_back(path, json::parse(text));
             state.busy = true;
         }},
        theme);
    (void)minimal_test::RenderFrame(form, 110, 30);
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::ArrowDown);   // questions
    minimal_test::SendEvent(form, ftxui::Event::Return);      // { action, tags }
    std::string frame = minimal_test::RenderFrame(form, 110, 30);
    CHECK(frame.find("{…} 2 keys") != std::string::npos);   // action: a nested object
    CHECK(frame.find("[…] 2 items") != std::string::npos);  // tags
    minimal_test::SendEvent(form, ftxui::Event::Return);      // action: { criteria, type }
    minimal_test::SendEvent(form, ftxui::Event::Return);      // criteria: { allow, deny }
    minimal_test::SendEvent(form, ftxui::Event::Return);      // allow: edit
    for (int i = 0; i < 4; ++i) minimal_test::SendEvent(form, ftxui::Event::Backspace);
    Type(form, "harmless");
    minimal_test::SendEvent(form, ftxui::Event::Return);
    minimal_test::SendEvent(form, ftxui::Event::Character('a'));   // a new member
    Type(form, "review");
    minimal_test::SendEvent(form, ftxui::Event::Return);
    Type(form, "unsure");
    minimal_test::SendEvent(form, ftxui::Event::Return);
    minimal_test::SendEvent(form, ftxui::Event::ArrowUp);      // from review (added last) to deny
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));
    minimal_test::SendEvent(form, ftxui::Event::Character('d'));   // deleted
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(form, ftxui::Event::Escape);   // up, up, out
    REQUIRE(sets.size() == 1);
    CHECK(sets[0].first == "questions");
    const json expect = {
        {"action", {{"type", "choice"}, {"criteria", {{"allow", "harmless"}, {"review", "unsure"}}}}},
        {"tags", {"a", "b"}}};
    CHECK(sets[0].second == expect);

    // `e`: the raw line, the escape hatch.
    state.busy = false;
    state.generation = 2;
    (void)minimal_test::RenderFrame(form, 110, 30);
    minimal_test::SendEvent(form, ftxui::Event::Character('e'));
    CHECK(minimal_test::RenderFrame(form, 110, 30).find("enter: save") != std::string::npos);
}
