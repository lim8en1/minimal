// session-view (plugins/client/session-view): the session and subagent
// pickers, over a fake host and server.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftxui_harness.hpp"
#include "minimal/client/policy.hpp"
#include "session_view.hpp"
#include "widget_kit_fixture.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

v1::SessionSummary Session(std::string id, std::string name, std::int64_t updated, bool live = false,
                           std::string parent = {}) {
    v1::SessionSummary s;
    s.set_session_id(std::move(id));
    s.set_name(std::move(name));
    s.set_updated_at_unix_ms(updated);
    s.set_is_live(live);
    s.set_parent_session_id(std::move(parent));
    return s;
}

class FakeHost final : public client::plugins::HostUi,
                       public client::plugins::HostActions,
                       public client::plugins::ClientApi,
                       public client::plugins::FocusProvider {
public:
    FakeHost() {
        // More than one page, newest first.
        for (int i = 0; i < client::kSessionListPageSize + 5; ++i) {
            stored.push_back(Session("s" + std::to_string(i), "session " + std::to_string(i),
                                     100000 - i, /*live=*/i == 1));
        }
    }

    void RequestRedraw() override {}
    void ShowView(const std::string&) override {}
    void CloseView(const std::string& id) override { Record("close " + id); }
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<client::plugins::KeyBindingInfo> KeyBindings() const override { return {}; }
    std::vector<client::CommandCatalogEntry> CommandCatalog() const override { return {}; }
    std::vector<std::string> ThemeNames() const override { return {}; }

    void InsertPrompt(const std::string&) override {}
    void SetTheme(const std::string&) override {}
    void ChooseAgent(const std::string&) override {}
    void ChooseModel(const std::string&) override {}
    void ChooseDefaultModel(const std::string&) override {}
    void SetSubagentModel(const std::string&) override {}
    void NewTab() override { Record("new"); }
    void OpenSession(const std::string& id, bool live) override {
        Record("open " + id + (live ? " live" : ""));
    }
    void SetClientPluginEnabled(const std::string&, bool,
                                std::function<void(const std::string&)>) override {}
    void OpenSubagent(const std::string& id, const std::string& parent, bool live) override {
        Record("subagent " + id + " of " + parent + (live ? " live" : ""));
    }

    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }
    std::optional<std::string> PluginCall(const std::string&, const std::string&,
                                          const std::string&) override {
        return std::nullopt;
    }
    std::optional<v1::ListModelsResponse> ListModels(bool, std::string*) override {
        return std::nullopt;
    }
    std::optional<v1::ListAgentPresetsResponse> ListAgentPresets(std::string*) override {
        return std::nullopt;
    }
    std::optional<v1::ListSessionsResponse> ListSessions(const SessionQuery& query,
                                                         std::string*) override {
        std::lock_guard<std::mutex> lock(mtx_);
        queries.push_back(query);
        v1::ListSessionsResponse response;
        int added = 0;
        for (const auto& s : stored) {
            if (query.before_updated_at && s.updated_at_unix_ms() >= *query.before_updated_at) continue;
            if (added == query.limit) break;
            *response.add_sessions() = s;
            ++added;
        }
        return response;
    }
    std::optional<std::vector<v1::SessionSummary>> ListSubagents(const std::string& parent,
                                                                 std::string*) override {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::vector<v1::SessionSummary>{Session("c1", "running child", 5, true, parent),
                                               Session("c2", "ended child", 4, false, parent)};
    }
    bool DeleteSession(const std::string& id, std::string*) override {
        std::lock_guard<std::mutex> lock(mtx_);
        std::erase_if(stored, [&](const v1::SessionSummary& s) { return s.session_id() == id; });
        log_.push_back("delete " + id);
        return true;
    }
    client::SubscribeOutcome PluginSubscribe(const std::string&, const std::string&,
                                             const std::string&,
                                             const std::function<bool(const std::string&)>&) override {
        return client::SubscribeOutcome::kError;
    }

    std::string FocusedSessionId() const override { return focused; }
    std::vector<client::plugins::TabInfo> Tabs() const override { return {}; }

    void Record(std::string entry) {
        std::lock_guard<std::mutex> lock(mtx_);
        log_.push_back(std::move(entry));
    }
    std::vector<std::string> log() {
        std::lock_guard<std::mutex> lock(mtx_);
        return log_;
    }
    session_view::Seams seams() { return {this, this, &minimal_test::CoreWidgetKit(), this, this}; }

    std::vector<v1::SessionSummary> stored;
    std::vector<SessionQuery> queries;
    std::string focused = "parent-1";

private:
    std::mutex mtx_;
    std::vector<std::string> log_;
};

client::Theme TestTheme() {
    return client::Theme::Parse(R"({"name":"test","colors":{}})", client::ColorDepth::Truecolor);
}

template <typename Pred>
bool Eventually(Pred pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("the sessions view: this project's first page, a new-session row, open by liveness",
          "[session_view]") {
    FakeHost host;
    session_view::SessionsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 40); };
    REQUIRE(Eventually([&] { return frame().find("session 0") != std::string::npos; }));
    CHECK(frame().find("more below") != std::string::npos);
    CHECK(frame().find("this project") != std::string::npos);
    REQUIRE_FALSE(host.queries.empty());
    CHECK_FALSE(host.queries[0].include_all_dirs);
    CHECK(host.queries[0].limit == client::kSessionListPageSize + 1);

    minimal_test::SendEvent(component, ftxui::Event::Return);  // "+ new session"
    CHECK(host.log() == std::vector<std::string>{"new", "close sessions"});
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // session 1: live
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.log().back() == "close sessions");
    CHECK(host.log()[host.log().size() - 2] == "open s1 live");
}

TEST_CASE("the sessions view: arrow-down past the last row loads the next page by cursor",
          "[session_view]") {
    FakeHost host;
    session_view::SessionsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 40); };
    REQUIRE(Eventually([&] { return frame().find("more below") != std::string::npos; }));
    for (int i = 0; i < client::kSessionListPageSize + 1; ++i) {
        minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    }
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // past the end: load more
    REQUIRE(Eventually([&] { return frame().find("more below") == std::string::npos; }));
    REQUIRE(host.queries.size() == 2);
    REQUIRE(host.queries[1].before_id.has_value());
    CHECK(*host.queries[1].before_id ==
          "s" + std::to_string(client::kSessionListPageSize - 1));  // the last row the server sent
}

TEST_CASE("the sessions view: ctrl+d asks first; Escape or n cancels, y deletes and refetches",
          "[session_view]") {
    FakeHost host;
    session_view::SessionsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    const auto* aware = dynamic_cast<const client::EscapeAware*>(component.get());
    REQUIRE(aware != nullptr);
    view.OnShow();
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 40); };
    REQUIRE(Eventually([&] { return frame().find("session 0") != std::string::npos; }));
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // session 0
    (void)frame();  // a frame between keys, as a terminal has: the cursor is remembered
    minimal_test::SendEvent(component, ftxui::Event::CtrlD);
    CHECK(frame().find("delete session?") != std::string::npos);
    CHECK(aware->WantsEscape());  // Escape is the confirmation's "cancel", not "close"
    minimal_test::SendEvent(component, ftxui::Event::Escape);
    CHECK(frame().find("resume a session") != std::string::npos);
    CHECK_FALSE(aware->WantsEscape());

    minimal_test::SendEvent(component, ftxui::Event::CtrlD);
    CHECK(frame().find("type to filter") == std::string::npos);  // a question, not a picker
    minimal_test::SendEvent(component, ftxui::Event::Character('n'));
    CHECK(frame().find("resume a session") != std::string::npos);

    minimal_test::SendEvent(component, ftxui::Event::CtrlD);
    (void)frame();
    // Enter answers nothing: one keystroke must not delete permanently.
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(frame().find("delete session?") != std::string::npos);
    CHECK((host.log().empty() || host.log().back() != "delete s0"));
    minimal_test::SendEvent(component, ftxui::Event::Character('y'));
    REQUIRE(Eventually([&] {
        const auto log = host.log();
        return !log.empty() && log.back() == "delete s0";
    }));
    REQUIRE(Eventually([&] {
        const std::string shown = frame();
        return shown.find("session 1") != std::string::npos &&
               shown.find("session 0 ") == std::string::npos;
    }));
}

TEST_CASE("the subagents view: the focused session's children, ended ones behind ctrl+a",
          "[session_view]") {
    FakeHost host;
    session_view::SubagentsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 30); };
    REQUIRE(Eventually([&] { return frame().find("running child") != std::string::npos; }));
    CHECK(frame().find("ended child") == std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::CtrlA);
    CHECK(frame().find("ended child") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.log() ==
          std::vector<std::string>{"subagent c1 of parent-1 live", "close subagents"});
}

TEST_CASE("the sessions view: a bare 'd' filters, tab sorts, ctrl+a asks for every project",
          "[session_view][regression]") {
    FakeHost host;
    session_view::SessionsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    const auto frame = [&] { return minimal_test::RenderFrame(component, 100, 40); };
    REQUIRE(Eventually([&] { return frame().find("session 0") != std::string::npos; }));

    // T8: delete moved to ctrl+d; a bare character types into the filter.
    minimal_test::SendEvent(component, ftxui::Event::Character("d"));
    CHECK(frame().find("delete session?") == std::string::npos);
    CHECK(frame().find("/ d") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::Backspace);

    CHECK(frame().find("sort: recent") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::Tab);
    CHECK(frame().find("sort: name") != std::string::npos);

    minimal_test::SendEvent(component, ftxui::Event::CtrlA);
    REQUIRE(Eventually([&] { return host.queries.size() == 2; }));
    CHECK(host.queries[1].include_all_dirs);
    CHECK_FALSE(host.queries[1].before_id.has_value());  // a fresh first page
    REQUIRE(Eventually([&] { return frame().find("all dirs") != std::string::npos; }));

    // A local filter hiding rows is not the server having more pages.
    minimal_test::SendEvent(component, ftxui::Event::Character("z"));
    for (int i = 0; i < 3; ++i) minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    std::this_thread::sleep_for(50ms);
    CHECK(host.queries.size() == 2);
}
