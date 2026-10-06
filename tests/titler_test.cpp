// session-titler, compiled into the test binary directly (the same pattern as
// the other bundled plugins: pure plugin logic over core headers, no dlopen).
//
// The plugin's dependency on core is ONE seam -- SessionSupportService, found
// through a ServiceRegistry -- so every test drives it through a fake of that
// interface, which is exactly what a third-party titler would be tested
// against.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/plugin_config_schema.hpp"
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
    // When set, served instead of `provider` (a provider the test controls
    // beyond scripted responses -- the gated one below).
    std::shared_ptr<llm::LlmProvider> provider_override;
    std::string model = "test-model";
    std::shared_ptr<const session::SessionReader> reader;
    bool serve_llm = true;
    bool fail_rename = false;
    // The titler's worker thread calls ResolveModelRef/RenameSession
    // while the test's main thread reads the records, so both vectors are
    // mutex-guarded and read only through the locked views below -- a data
    // race here is a test bug, not a plugin finding.
    std::vector<std::pair<std::string, std::string>> renames;  // (session_id, name)
    std::vector<std::string> resolved_refs;

    std::optional<agent::SessionLlm> LlmForSession(const std::string&) override {
        if (!serve_llm) return std::nullopt;
        return Served();
    }
    std::optional<agent::SessionLlm> ResolveModelRef(const std::string& ref) override {
        std::lock_guard<std::mutex> lock(records_mu);
        resolved_refs.push_back(ref);
        if (!serve_llm) return std::nullopt;
        return Served();
    }
    agent::SessionLlm Served() const {
        if (provider_override) return agent::SessionLlm{provider_override, model};
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

// A real database holding one fresh session whose first user message is
// `first_text` -- what ProcessJob reads through HistoryFor.
struct Transcript {
    testing::TempDir dir;
    session::SessionDatabase db{dir.path() / "sessions.db"};
    std::shared_ptr<const session::SessionReader> reader =
        std::make_shared<const session::SessionReader>(db);

    std::string AddSession(const std::string& first_text) {
        const auto id = db.CreateSession("", std::nullopt, "/p", "build").session_id;
        db.AppendMessage(id, testing::UserText(first_text));
        return id;
    }
};

// Blocks every Complete until Release(); counts calls. The destructor test's
// "a provider call is in flight" state.
class GatedProvider final : public llm::LlmProvider {
public:
    llm::ChatResponse Complete(const llm::ChatRequest& request) override {
        return StreamComplete(request, {});
    }
    llm::ChatResponse StreamComplete(const llm::ChatRequest&,
                                     const llm::StreamEventSink&) override {
        std::unique_lock<std::mutex> lock(mu_);
        ++calls_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
        return testing::TextResponse("Gated title");
    }
    void WaitForCall() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, 5s, [this] { return calls_ > 0; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mu_);
        released_ = true;
        cv_.notify_all();
    }
    int calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    int calls_ = 0;
    bool released_ = false;
};

// The worker is asynchronous; the failure mode is "never", so poll with a
// deadline -- a hang here is the bug, not a wrong assertion.
void WaitForRenames(const FakeSupportService& support, std::size_t n) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (support.Renames().size() < n && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

TEST_CASE("the titler's model field is a model ref, empty meaning the session's own",
          "[titler][config_schema]") {
    // What lets /config offer the server's models instead of free text.
    const auto schema = minimal::config::DescribeConfig(
        "plugins.d/session-titler.json",
        [](const nlohmann::json& config) { (void)session_titler::ParseTitlerConfig(config); });
    const nlohmann::json* model = nullptr;
    for (const auto& field : schema.at("fields")) {
        if (field.at("path") == "model") model = &field;
    }
    REQUIRE(model != nullptr);
    CHECK(model->at("format") == "model_ref");
    CHECK(model->at("empty_label") == "the session's own model");
    for (const auto& field : schema.at("fields")) {
        if (field.at("path") != "model") CHECK_FALSE(field.contains("format"));
    }
}

TEST_CASE("ParseTitlerConfig honors overrides and refuses junk by name", "[titler]") {
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

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(session_titler::ParseTitlerConfig({{"max_tokens", -5}}),
                      ContainsSubstring("plugins.d/session-titler.json: max_tokens: must be at least 1"));
    CHECK_THROWS_WITH(session_titler::ParseTitlerConfig({{"input_char_budget", "lots"}}),
                      ContainsSubstring("input_char_budget: must be an integer"));
    CHECK_THROWS_WITH(session_titler::ParseTitlerConfig({{"model", "cheap"}}),
                      ContainsSubstring("model: must be <provider>/<model>"));
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

TEST_CASE("CleanTitle drops control bytes instead of rendering them", "[titler][invariant]") {
    using session_titler::CleanTitle;
    // An ESC (ANSI escape) and other C0 controls in a model reply used to
    // reach the tab bar verbatim.
    CHECK(CleanTitle("\x1B" "[31mred bug") == "[31mred bug");
    CHECK(CleanTitle("fix\x01" "the\x7F" "bug") == "fixthebug");
    // Whitespace semantics unchanged.
    CHECK(CleanTitle("fix\tthe\r\nbug") == "fix the bug");
}

TEST_CASE("CleanTitle strips the decoration models add anyway", "[titler]") {
    using session_titler::CleanTitle;
    CHECK(CleanTitle("Fix the login bug.") == "Fix the login bug");
    CHECK(CleanTitle("Wait for it...") == "Wait for it...");  // an ellipsis stays
    CHECK(CleanTitle("Title: Fix the login bug") == "Fix the login bug");
    CHECK(CleanTitle("TITLE:Fix it") == "Fix it");
    CHECK(CleanTitle("# Fix the login bug") == "Fix the login bug");
    CHECK(CleanTitle("**Fix the login bug**") == "Fix the login bug");
    // Nested layers peel together.
    CHECK(CleanTitle("**\"Fix the login bug.\"**") == "Fix the login bug");
    CHECK(CleanTitle("Title: \"Fix it\"") == "Fix it");
    // Inner markup is content, not decoration.
    CHECK(CleanTitle("Fix *the* bug") == "Fix *the* bug");
    CHECK(CleanTitle("**").empty());
}

TEST_CASE("ProcessJob titles from the session's own model by default", "[titler]") {
    Transcript transcript;
    const auto id = transcript.AddSession("please fix the login bug");
    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = transcript.reader;
    services.Provide(agent::kSessionSupportServiceName, &support);
    support.provider->PushResponse(testing::TextResponse("  \"Fix the login bug\"  "));

    session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                            common::DefaultLogger());
    observer.ProcessJob(id);

    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first == id);
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

TEST_CASE("ProcessJob truncates the first user text to the input budget", "[titler]") {
    Transcript transcript;
    std::string long_text;
    for (int i = 0; i < 20; ++i) long_text += "\xC3\xA9";  // é × 20
    const auto id = transcript.AddSession(long_text);
    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = transcript.reader;
    services.Provide(agent::kSessionSupportServiceName, &support);
    support.provider->PushResponse(testing::TextResponse("t"));

    session_titler::TitlerConfig config;
    config.input_char_budget = 5;
    session_titler::TitlerObserver observer(config, &services, common::DefaultLogger());
    observer.ProcessJob(id);

    REQUIRE(support.provider->requests().size() == 1);
    // 5 code points, 10 bytes -- never a split sequence.
    CHECK(std::get<llm::TextBlock>(support.provider->requests()[0].messages[0].content[0])
              .text == long_text.substr(0, 10));
}

TEST_CASE("ProcessJob routes a configured model override through ResolveModelRef",
          "[titler]") {
    Transcript transcript;
    const auto id = transcript.AddSession("text");
    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = transcript.reader;
    services.Provide(agent::kSessionSupportServiceName, &support);
    support.provider->PushResponse(testing::TextResponse("tiny title"));

    session_titler::TitlerConfig config;
    config.model = "cheap/main";
    session_titler::TitlerObserver observer(config, &services, common::DefaultLogger());
    observer.ProcessJob(id);

    const auto refs = support.ResolvedRefs();
    REQUIRE(refs.size() == 1);
    CHECK(refs[0] == "cheap/main");
    // And the title still lands.
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].second == "tiny title");
}

TEST_CASE("ProcessJob degrades on every failure path", "[titler]") {
    Transcript transcript;
    const auto id = transcript.AddSession("text");
    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = transcript.reader;

    SECTION("no service from the registry") {
        services.Seal();  // sealed-empty: Find answers null
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);  // must not crash or rename
    }
    SECTION("no service pointer at all (host offers no seam)") {
        session_titler::TitlerObserver observer(DefaultConfig(), nullptr,
                                                common::DefaultLogger());
        observer.ProcessJob(id);
    }
    SECTION("no transcript") {
        support.reader = nullptr;
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);
        CHECK(support.provider->call_count() == 0);  // nothing spent
        CHECK(support.Renames().empty());
    }
    SECTION("no provider resolvable") {
        support.serve_llm = false;
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);
        CHECK(support.Renames().empty());
    }
    SECTION("provider error") {
        support.provider->PushError(
            llm::LlmError(llm::LlmErrorCategory::Transport, "connection refused"));
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);  // caught: never escapes the worker
        CHECK(support.Renames().empty());
    }
    SECTION("empty model output") {
        support.provider->PushResponse(testing::TextResponse(""));
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);
        CHECK(support.Renames().empty());
    }
    SECTION("max_tokens spent before any visible text (reasoning model)") {
        support.provider->PushResponse(
            testing::TextResponse("", llm::StopReason::MaxTokens));
        services.Provide(agent::kSessionSupportServiceName, &support);
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        observer.ProcessJob(id);  // warns; keeps the default name
        CHECK(support.Renames().empty());
    }
}

TEST_CASE("OnTurnCompleted skips everything that is not a fresh session's first turn",
          "[titler]") {
    plugins::ServiceRegistry services;
    FakeSupportService support;
    services.Provide(agent::kSessionSupportServiceName, &support);
    {
        session_titler::TitlerObserver observer(DefaultConfig(), &services,
                                                common::DefaultLogger());
        // Subagents are named by their task.
        observer.OnTurnCompleted(Info("child", "parent"), Summary(0));
        // A resumed session (or wake turn): the first new seq is above the seed.
        observer.OnTurnCompleted(Info("s1"), Summary(7));
    }  // joined: anything enqueued would have run or been dropped by now
    CHECK(support.provider->call_count() == 0);
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
    observer.OnTurnCompleted(Info(id), Summary(0));
    WaitForRenames(support, 1);
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first == id);
    CHECK(renames[0].second == "Fix the login bug");

    // A second first-turn-shaped notification is still one title.
    observer.OnTurnCompleted(Info(id), Summary(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(support.Renames().size() == 1);
    CHECK(support.provider->call_count() == 1);

    observer.OnSessionEnded(Info(id));
}

TEST_CASE("The destructor waits for the in-flight title and drops queued ones",
          "[titler][async]") {
    // Shutdown contract: the host releases observers while the support service
    // is still alive, so the one in-flight provider call is waited on (its
    // rename must not outlive the observer), but queued titles are dropped --
    // exit must not wait on N provider calls.
    Transcript transcript;
    const auto first = transcript.AddSession("first");
    const auto second = transcript.AddSession("second");
    plugins::ServiceRegistry services;
    FakeSupportService support;
    support.reader = transcript.reader;
    auto gated = std::make_shared<GatedProvider>();
    support.provider_override = gated;
    services.Provide(agent::kSessionSupportServiceName, &support);

    auto observer = std::make_unique<session_titler::TitlerObserver>(
        DefaultConfig(), &services, common::DefaultLogger());
    observer->OnTurnCompleted(Info(first), Summary(0));
    gated->WaitForCall();  // `first` is in flight
    REQUIRE(gated->calls() == 1);
    observer->OnTurnCompleted(Info(second), Summary(0));  // queued behind it

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        observer.reset();
        destroyed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(destroyed.load());  // still joined on the in-flight call
    gated->Release();
    destroyer.join();

    CHECK(gated->calls() == 1);  // `second` never reached the provider
    const auto renames = support.Renames();
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first == first);
    CHECK(renames[0].second == "Gated title");
}
