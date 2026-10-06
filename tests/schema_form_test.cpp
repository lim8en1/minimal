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
    client::SchemaFormState state = FormState("plugins.d/sample.json", kDescribe);
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
    auto form = client::MakeSchemaForm(
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
    auto form = client::MakeSchemaForm([&] { return state; }, {}, theme);
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
    auto form = client::MakeSchemaForm(
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
    auto form = client::MakeSchemaForm([&] { return state; }, {}, theme);
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
    auto form = client::MakeSchemaForm(
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
    auto form = client::MakeSchemaForm(
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
        if (method == "set" && refuse_) return json{{"error", "limit: must be at least 1"}}.dump();
        if (method == "set") return json{{"applied", "live"}, {"changed", {"net.timeout_ms"}}}.dump();
        return std::nullopt;
    }
    std::optional<minimal::v1::ListModelsResponse> ListModels(bool, std::string*) override {
        minimal::v1::ListModelsResponse response;
        for (const char* ref : {"main/alpha", "main/beta"}) response.add_models()->set_ref(ref);
        return response;
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
    CHECK_FALSE(aware->WantsEscape());

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
    CHECK(frame().find("timeout_ms") != std::string::npos);
    CHECK_FALSE(aware->WantsEscape());
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
