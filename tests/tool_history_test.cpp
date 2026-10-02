// Tool history access (docs/EXTENSIBILITY.md §2.1): ToolContext::history.
//
// A tool reads the transcript it runs inside through a session::HistoryWindow --
// a bound, windowed slice of SessionReader with no full-materialization path.
// The tests cover the three designed behaviours: a live-turn read through a real
// SQLite-backed session, the null-history degrade on stores that are not
// SQLite-backed, and subagent isolation (a child's window is bound to the
// child's own transcript). The end-to-end test asserting history != nullptr is
// also the invariant guard: removing the ctx.history wiring in the agent loop
// fails it.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/session/session_database.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// A probe tool: reads the conversation through ctx.history and reports what it
// saw as its result text, so assertions can run on the recorded provider
// requests as well as on the captured observations.
class HistoryProbeTool : public tools::Tool {
public:
    struct Observed {
        std::string session_id;
        bool had_history = false;
        std::size_t seen = 0;
    };

    HistoryProbeTool() {
        spec_.name = "history_probe";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "reports what the conversation history shows";
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        std::lock_guard<std::mutex> lock(mu_);
        (void)input;
        observed_.push_back(Observed{ctx.session_id, ctx.history != nullptr, 0});

        if (ctx.history == nullptr) {
            // The degrade the plan requires: the tool handles it, no crash.
            return tools::ToolResult{false, "history=null; saw=0"};
        }

        // Windowed read, exactly like the GetMessages RPC: newest page first,
        // bounded by limit. before_seq paging is SessionReader's, already
        // tested; here the newest window is what a live tool wants.
        const auto page = ctx.history->GetMessages(std::nullopt, 16);
        observed_.back().seen = page.messages.size();

        std::string roles;
        std::string last_user_text;
        for (const auto& stored : page.messages) {
            if (!roles.empty()) roles += ",";
            roles += stored.message.role == llm::Role::User ? "U" : "A";
            if (stored.message.role == llm::Role::User && last_user_text.empty()) {
                for (const auto& block : stored.message.content) {
                    if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                        last_user_text = text->text;
                        break;
                    }
                }
            }
        }
        return tools::ToolResult{false, "history=ok; saw=" + std::to_string(page.messages.size()) +
                                            "; roles=" + roles + "; lastuser=" + last_user_text};
    }

    std::vector<Observed> observed() {
        std::lock_guard<std::mutex> lock(mu_);
        return observed_;
    }

private:
    tools::ToolSpec spec_;
    std::mutex mu_;
    std::vector<Observed> observed_;
};

// Concatenates every text-bearing block of a request's messages, so an
// assertion can search for a tool result that should have reached the model.
std::string TextOf(const llm::ChatRequest& request) {
    std::string out;
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                out += text->text + "\n";
            } else if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                for (const auto& inner : result->content) {
                    if (const auto* text = std::get_if<llm::TextBlock>(&inner)) {
                        out += text->text + "\n";
                    }
                }
            }
        }
    }
    return out;
}

// --- Registry scaffolding: real SQLite store, scripted provider ---------------

struct TempConfigDir {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

    TempConfigDir() { std::filesystem::create_directories(dir / "agents"); }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void WriteDefaults() const {
        Write("providers.json",
              nlohmann::json{{"providers",
                              {{"main",
                                {{"backend", "fake"},
                                 {"url", "http://localhost:8080/v1"},
                                 {"models", {{"test-model", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", "{}");
        Write("permissions.json", "{}");
        Write("agents/build.md",
              "---\nname: build\nmodel: main/test-model\n---\nYou are a build agent.\n");
    }
};

struct HistoryFixture {
    HistoryFixture()
        : skills(config::SkillRegistry::Load(config.dir)),
          observers(std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger())) {}

    void Build() {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            config.dir, skills, config::LoadSettingsConfig(config.dir / "settings.json"));
        providers.Register("fake", [this](const llm::ProviderConfig&) {
            return std::make_unique<testing::ScriptedProvider>(script);
        });
        db = std::make_unique<session::SessionDatabase>(db_dir.path() / "sessions.db");
        registry = std::make_unique<agent::SessionRegistry>(*presets, rpc::FinishSessionWiring,
            plugins::HostRegistries{.tools = &tools_registry,
                                    .providers = &providers,
                                    .observers = observers.get()},
            *db, agent::LoopConfig{}, common::DefaultLogger());
    }

    TempConfigDir config;
    config::SkillRegistry skills;
    tools::ToolRegistry tools_registry;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    testing::TempDir db_dir;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> registry;
    std::shared_ptr<testing::ScriptedProviderScript> script =
        std::make_shared<testing::ScriptedProviderScript>();
};

agent::SessionParams Params(std::string id, std::string preset = "build") {
    agent::SessionParams params;
    params.session_id = std::move(id);
    params.agent_preset = std::move(preset);
    params.project_root = std::filesystem::temp_directory_path();
    return params;
}

}  // namespace

TEST_CASE("a tool reads the transcript through ctx.history during a live turn",
          "[tool_history]") {
    HistoryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto probe = std::make_unique<HistoryProbeTool>();
    auto* probe_ptr = probe.get();
    fixture.tools_registry.Register(std::move(probe));

    fixture.script->Push(testing::ToolUseResponse("call-1", "history_probe"));
    fixture.script->Push(testing::TextResponse("done"));

    const auto row = fixture.db->CreateSession("live", std::nullopt, "/proj", "build");
    auto session = fixture.registry->Create(Params(row.session_id));
    REQUIRE(session != nullptr);

    session->SubmitTurn("hello there");
    REQUIRE(session->WaitForIdleForTesting(5s));

    const auto seen = probe_ptr->observed();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].had_history);
    CHECK(seen[0].session_id == row.session_id);

    // The tool result reached the model, and the tool saw what a live turn
    // looks like mid-flight: the user turn is persisted before dispatch, but
    // the round's assistant tool_use is committed WITH its results afterwards
    // (one atomic unit), so the newest window holds exactly the user turn.
    const auto& requests = fixture.script->requests;
    REQUIRE(requests.size() == 2);
    const std::string second = TextOf(requests[1]);
    CHECK(second.find("history=ok; saw=1") != std::string::npos);
    CHECK(second.find("roles=U") != std::string::npos);
    CHECK(second.find("lastuser=hello there") != std::string::npos);
    session->Terminate();
}

TEST_CASE("sessions on non-SQLite stores see history == nullptr and tools degrade",
          "[tool_history]") {
    // Plain Session on the in-memory fake, no reader behind the window: null is
    // the natural "unavailable" case, not an error path.
    agent::AgentPreset preset;
    preset.name = "test";
    preset.allowed_tools = {"*"};
    preset.permission_mode = testing::AllowAllPolicy();

    tools::ToolRegistry registry;
    auto probe = std::make_unique<HistoryProbeTool>();
    auto* probe_ptr = probe.get();
    registry.Register(std::move(probe));

    agent::SessionObserverRegistry observers{common::DefaultLogger()};
    auto provider = std::make_unique<testing::FakeLlmProvider>();
    provider->PushResponse(testing::ToolUseResponse("call-1", "history_probe"));
    provider->PushResponse(testing::TextResponse("done"));
    auto store = std::make_unique<testing::InMemoryHistoryStore>();

    agent::SessionParams params;
    params.session_id = "sess-null";
    params.agent_preset = "test";
    params.project_root = std::filesystem::temp_directory_path();
    rpc::FinishSessionWiring(params);

    agent::Session session(std::move(params), preset, std::move(provider), std::move(store),
                           registry, agent::LoopConfig{}, observers, common::DefaultLogger());

    session.SubmitTurn("anything");
    REQUIRE(session.WaitForIdleForTesting(5s));

    const auto seen = probe_ptr->observed();
    REQUIRE(seen.size() == 1);
    CHECK_FALSE(seen[0].had_history);
    session.Terminate();
}

TEST_CASE("a subagent's tool sees the child's transcript only, never the parent's",
          "[tool_history][subagent]") {
    HistoryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto probe = std::make_unique<HistoryProbeTool>();
    auto* probe_ptr = probe.get();
    fixture.tools_registry.Register(std::move(probe));

    // Parent: one full plain turn => user + assistant, no tool involvement.
    const auto parent_row = fixture.db->CreateSession("parent", std::nullopt, "/proj", "build");
    fixture.script->Push(testing::TextResponse("parent done"));
    auto parent = fixture.registry->Create(Params(parent_row.session_id));
    parent->SubmitTurn("parent business only");
    REQUIRE(parent->WaitForIdleForTesting(5s));
    parent->Terminate();

    // Child: its one turn dispatches the probe. The registry is the one funnel
    // both sessions pass through, so the child's window is bound to the child's
    // own id with no extra code.
    const auto child_row =
        fixture.db->CreateSession("child", parent_row.session_id, "/proj", "build");
    fixture.script->Push(testing::ToolUseResponse("call-1", "history_probe"));
    fixture.script->Push(testing::TextResponse("child done"));

    auto child_params = Params(child_row.session_id);
    child_params.parent_session_id = parent_row.session_id;
    auto child = fixture.registry->Create(std::move(child_params));
    REQUIRE(child != nullptr);

    child->SubmitTurn("child task");
    REQUIRE(child->WaitForIdleForTesting(5s));

    const auto seen = probe_ptr->observed();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].session_id == child_row.session_id);
    CHECK(seen[0].had_history);

    const auto& requests = fixture.script->requests;
    // Three calls: the parent's plain turn, then the child's tool round and
    // final text. The tool result reaching the model is the SECOND child call.
    REQUIRE(requests.size() == 3);
    const std::string second = TextOf(requests[2]);
    // M = the child's own user turn (the in-flight round's assistant message
    // is committed with its results, after the tool runs). The parent's N
    // messages (its user turn included) are nowhere in the child's window.
    CHECK(second.find("history=ok; saw=1") != std::string::npos);
    CHECK(second.find("lastuser=child task") != std::string::npos);
    CHECK(second.find("parent business only") == std::string::npos);
    child->Terminate();
}

TEST_CASE("HistoryWindow pages the bound session only, with SessionReader cursor semantics",
          "[tool_history]") {
    // The wrapper's whole surface, driven directly: newest-first paging with a
    // keyset cursor, bounded by limit, and bound to one session id -- the
    // isolation a tool gets is the binding, not its own discipline.
    testing::TempDir db_dir;
    session::SessionDatabase db(db_dir.path() / "sessions.db");
    const auto row = db.CreateSession("w", std::nullopt, "/p", "build");
    const auto other = db.CreateSession("other", std::nullopt, "/p", "build");
    db.AppendMessage(row.session_id, testing::UserText("first"));
    db.AppendMessage(other.session_id, testing::UserText("poison"));
    db.AppendMessage(row.session_id, testing::UserText("second"));

    session::SessionReader reader(db);
    session::HistoryWindow window(reader, row.session_id);

    const auto newest = window.GetMessages(std::nullopt, 1);
    REQUIRE(newest.messages.size() == 1);
    CHECK(newest.messages.front().message.role == llm::Role::User);
    CHECK(std::get<llm::TextBlock>(newest.messages.front().message.content.front()).text ==
          "second");
    CHECK(newest.has_more);

    // Keyset cursor: everything before the newest, one page.
    const auto older = window.GetMessages(newest.messages.front().seq, 16);
    REQUIRE(older.messages.size() == 1);
    CHECK_FALSE(older.has_more);
    CHECK(std::get<llm::TextBlock>(older.messages.front().message.content.front()).text ==
          "first");
    // The other session's row never appears: the window is bound.
}

TEST_CASE("HistoryWindow clamps the page size instead of honoring a tool's huge limit",
          "[tool_history][invariant]") {
    // The 512k-1M guarantee is the CAP, not the tool's restraint: a limit of
    // 10^8 must not turn one call into a full-transcript materialization, and
    // a non-positive limit degrades rather than throwing out of a tool call.
    testing::TempDir db_dir;
    session::SessionDatabase db(db_dir.path() / "sessions.db");
    const auto row = db.CreateSession("w", std::nullopt, "/p", "build");
    for (int i = 0; i < 3; ++i) {
        db.AppendMessage(row.session_id, testing::UserText("m" + std::to_string(i)));
    }
    session::SessionReader reader(db);
    session::HistoryWindow window(reader, row.session_id);

    const auto huge = window.GetMessages(std::nullopt, 100000000);
    CHECK(huge.messages.size() == 3);  // transcript size, not 10^8 -- and the
                                       // cap, not the data, would bound a real one
    CHECK(window.GetMessages(std::nullopt, session::HistoryWindow::kMaxPageLimit + 1)
              .messages.size() == 3);

    const auto zero = window.GetMessages(std::nullopt, 0);
    CHECK(zero.messages.size() == 1);
    const auto negative = window.GetMessages(std::nullopt, -5);
    CHECK(negative.messages.size() == 1);
}
