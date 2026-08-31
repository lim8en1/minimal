// session-titler, compiled into the test binary directly (the same pattern as
// the other bundled plugins: pure plugin logic over core headers, no dlopen).
//
// The plugin's dependency on core is ONE seam -- SessionSupportService, found
// through a ServiceRegistry -- so every test drives it through a fake of that
// interface, which is exactly what a third-party titler would be tested
// against.

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/session_reader.hpp"

#include "fakes.hpp"
#include "temp_dir.hpp"
#include "titler.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// Records what the plugin does; hands out a scripted provider and an optional
// real SessionReader over a real database.
class FakeSupportService final : public agent::SessionSupportService {
public:
    std::shared_ptr<testing::FakeLlmProvider> provider =
        std::make_shared<testing::FakeLlmProvider>();
    std::string model = "test-model";
    std::shared_ptr<const session::SessionReader> reader;
    bool serve_llm = true;
    bool fail_rename = false;
    // The titler's detached worker thread calls ResolveModelRef/RenameSession
    // while the test's main thread reads the records, so both vectors are
    // mutex-guarded and read only through the locked views below -- a data
    // race here is a test bug, not a plugin finding.
    std::vector<std::pair<std::string, std::string>> renames;  // (session_id, name)
    std::vector<std::string> resolved_refs;

    std::optional<agent::SessionLlm> LlmForSession(const std::string&) override {
        if (!serve_llm) return std::nullopt;
        return agent::SessionLlm{provider, model};
    }
    std::optional<agent::SessionLlm> ResolveModelRef(const std::string& ref) override {
        std::lock_guard<std::mutex> lock(records_mu);
        resolved_refs.push_back(ref);
        if (!serve_llm) return std::nullopt;
        return agent::SessionLlm{provider, model};
    }
    std::shared_ptr<const session::SessionReader> HistoryFor(const std::string&) override {
        return reader;
    }
    bool RenameSession(const std::string& session_id, const std::string& name) override {
        if (fail_rename) return false;
        std::lock_guard<std::mutex> lock(records_mu);
        renames.push_back({session_id, name});
        return true;
    }

    // Locked read views: the main thread must not touch the raw vectors.
    std::vector<std::pair<std::string, std::string>> Renames() const {
        std::lock_guard<std::mutex> lock(records_mu);
        return renames;
    }
    std::vector<std::string> ResolvedRefs() const {
        std::lock_guard<std::mutex> lock(records_mu);
        return resolved_refs;
    }

private:
    mutable std::mutex records_mu;
};

agent::SessionInfo Info(std::string id, std::string parent = "") {
    agent::SessionInfo info;
    info.session_id = std::move(id);
    info.parent_session_id = std::move(parent);
    info.project_root = "/p";
    return info;
}

agent::TurnSummary Summary(std::int64_t first_seq) {
    agent::TurnSummary summary;
    summary.first_seq = first_seq;
    summary.last_seq = first_seq + 1;
    summary.stop_reason = llm::StopReason::EndTurn;
    return summary;
}

session_titler::TitlerConfig DefaultConfig() { return {}; }

}  // namespace

TEST_CASE("ParseTitlerConfig is lenient and honors sane overrides", "[titler]") {
    // Missing config: every default stands.
    const auto defaults = session_titler::ParseTitlerConfig(nlohmann::json::object());
    CHECK(defaults.model.empty());
    CHECK(defaults.max_tokens == 64);
    CHECK(defaults.input_char_budget == 2000);
    CHECK(defaults.prompt.empty());

    const auto configured = session_titler::ParseTitlerConfig(nlohmann::json{
        {"model", "cheap/main"},
        {"max_tokens", 32},
        {"input_char_budget", 500},
        {"prompt", "Title it."},
    });
    CHECK(configured.model == "cheap/main");
    CHECK(configured.max_tokens == 32);
    CHECK(configured.input_char_budget == 500);
    CHECK(configured.prompt == "Title it.");

    // Junk values fall back to defaults rather than disabling the plugin.
    const auto junk = session_titler::ParseTitlerConfig(nlohmann::json{
        {"max_tokens", -5}, {"input_char_budget", "lots"}, {"model", 7}});
    CHECK(junk.model.empty());
    CHECK(junk.max_tokens == 64);
    CHECK(junk.input_char_budget == 2000);

    // A non-object config degrades to defaults.
    CHECK(session_titler::ParseTitlerConfig(nlohmann::json::array()).max_tokens == 64);
}

TEST_CASE("CleanTitle produces a tab-label-shaped title", "[titler]") {
    using session_titler::CleanTitle;
    CHECK(CleanTitle("  Fix the login bug  ") == "Fix the login bug");
    // One wrapping pair of quotes comes off; inner ones stay.
    CHECK(CleanTitle("\"Fix the 'login' bug\"") == "Fix the 'login' bug");
    CHECK(CleanTitle("'Fix the bug'") == "Fix the bug");
    // Newline runs collapse: a multi-line reply is one label.
    CHECK(CleanTitle("Fix\n\nthe\n\tbug") == "Fix the bug");
    CHECK(CleanTitle("") .empty());
    CHECK(CleanTitle("   ").empty());

    // 60 code points, never splitting a UTF-8 sequence (the same cap the free
    // default name uses).
    std::string long_unicode;
    for (int i = 0; i < 70; ++i) long_unicode += "\xC3\xA9";  // é × 70
    CHECK(CleanTitle(long_unicode) == long_unicode.substr(0, 120));  // 60 × 2 bytes
    CHECK(CleanTitle(std::string(70, 'a')).size() == 60);
}

TEST_CASE("RunJobNow titles from the session's own model by default", "[titler]") {
    plugins::ServiceRegistry services;
    FakeSupportService support;
    services.Provide(agent::kSessionSupportServiceName, &support);
    support.provider->PushResponse(testing::TextResponse("  \"Fix the login bug\"  "));

    session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                            common::DefaultLogger());
    observer.RunJobNow("s1", "please fix the login bug");

    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first == "s1");
    CHECK(renames[0].second == "Fix the login bug");

    // The request went to the session's model, with the default prompt and cap.
    REQUIRE(support.provider->requests().size() == 1);
    const auto& request = support.provider->requests()[0];
    CHECK(request.model == "test-model");
    CHECK(request.max_tokens == 64);
    CHECK(request.system.has_value());
    CHECK(*request.system == session_titler::DefaultPrompt());
    REQUIRE(request.messages.size() == 1);
    CHECK(std::get<llm::TextBlock>(request.messages[0].content[0]).text ==
          "please fix the login bug");
}

TEST_CASE("RunJobNow routes a configured model override through ResolveModelRef",
          "[titler]") {
    plugins::ServiceRegistry services;
    FakeSupportService support;
    services.Provide(agent::kSessionSupportServiceName, &support);
    support.provider->PushResponse(testing::TextResponse("tiny title"));

    session_titler::TitlerConfig config;
    config.model = "cheap/main";
    session_titler::TitlerObserver observer(config, &services, common::DefaultLogger());
    observer.RunJobNow("s1", "text");

    const auto refs = support.ResolvedRefs();
    REQUIRE(refs.size() == 1);
    CHECK(refs[0] == "cheap/main");
    // And the title still lands.
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].second == "tiny title");
}

TEST_CASE("RunJobNow degrades on every failure path", "[titler]") {
    SECTION("no service from the registry") {
        plugins::ServiceRegistry services;  // sealed-empty: Find answers null
        services.Seal();
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.RunJobNow("s1", "text");  // must not crash or rename
    }
    SECTION("no provider resolvable") {
        plugins::ServiceRegistry services;
        FakeSupportService support;
        support.serve_llm = false;
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.RunJobNow("s1", "text");
        CHECK(support.Renames().empty());
    }
    SECTION("provider error") {
        plugins::ServiceRegistry services;
        FakeSupportService support;
        support.provider->PushError(
            llm::LlmError(llm::LlmErrorCategory::Transport, "connection refused"));
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.RunJobNow("s1", "text");
        CHECK(support.Renames().empty());
    }
    SECTION("empty model output") {
        plugins::ServiceRegistry services;
        FakeSupportService support;
        support.provider->PushResponse(testing::TextResponse(""));
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.RunJobNow("s1", "text");
        CHECK(support.Renames().empty());
    }
    SECTION("no service pointer at all (host offers no seam)") {
        session_titler::TitlerObserver observer(DefaultConfig(), nullptr,
                                                common::DefaultLogger());
        observer.RunJobNow("s1", "text");  // degrade silently
    }
}

TEST_CASE("OnTurnCompleted skips everything that is not a fresh session's first turn",
          "[titler]") {
    plugins::ServiceRegistry services;
    FakeSupportService support;
    services.Provide(agent::kSessionSupportServiceName, &support);
    session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                            common::DefaultLogger());

    // Subagents are named by their task.
    observer.OnTurnCompleted(Info("child", "parent"), Summary(0));
    CHECK(support.Renames().empty());

    // A resumed session (or wake turn): the first new seq is above the seed.
    observer.OnTurnCompleted(Info("s1"), Summary(7));
    CHECK(support.Renames().empty());
}

TEST_CASE("OnTurnCompleted reads the first user text and titles asynchronously",
          "[titler][async]") {
    testing::TempDir dir;
    session::SessionDatabase db(dir.path() / "sessions.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, testing::UserText("please fix the login bug"));
    db.AppendMessage(id, testing::UserText(""));  // a tool-result-only message

    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = std::make_shared<const session::SessionReader>(db);
    support.provider->PushResponse(testing::TextResponse("\"Fix the login bug\""));
    services.Provide(agent::kSessionSupportServiceName, &support);

    session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                            common::DefaultLogger());
    // The production path: the job runs on a detached thread, so the rename
    // lands later. Poll with a deadline -- the failure mode is "never", and a
    // hang here is the bug, not a wrong assertion.
    observer.OnTurnCompleted(Info(id), Summary(0));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (support.Renames().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first == id);
    CHECK(renames[0].second == "Fix the login bug");

    // A second first-turn-shaped notification is still one title, ever.
    observer.OnTurnCompleted(Info(id), Summary(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(support.Renames().size() == 1);

    observer.OnSessionEnded(Info(id));
}

TEST_CASE("OnTurnCompleted resolves the provider before going async", "[titler][async]") {
    // THE TERMINATION RACE, as a regression test: the client closes its stream
    // at turn_completed, the session terminates and leaves the registry, and a
    // titler that resolved its provider on the DETACHED thread would lose. The
    // provider must be captured on the observer thread -- so once
    // OnTurnCompleted returns, the service may stop serving the live session
    // entirely and the title must still land.
    testing::TempDir dir;
    session::SessionDatabase db(dir.path() / "sessions.db");
    const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
    db.AppendMessage(id, testing::UserText("please fix the login bug"));

    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = std::make_shared<const session::SessionReader>(db);
    support.provider->PushResponse(testing::TextResponse("Fix the login bug"));
    services.Provide(agent::kSessionSupportServiceName, &support);

    session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                            common::DefaultLogger());
    observer.OnTurnCompleted(Info(id), Summary(0));
    // The session is "gone" the moment the callback returned.
    support.serve_llm = false;

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (support.Renames().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].second == "Fix the login bug");
}
