#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "minimal/agent/session.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/subagent_manager.hpp"
#include "minimal/agent/subagent_spawn.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/mcps_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "support/fakes.hpp"
#include "support/session_events.hpp"
#include "support/temp_dir.hpp"

using namespace std::chrono_literals;
using namespace minimal;

// Session-level MCP integration: the liveness check at session start, the
// surfaced error for a dead server, and the tools the model actually sees.
// In-process (no gRPC); the wire tier is mcp_e2e_test.cpp.

namespace {

constexpr const char* kFakeServer = MINIMAL_MCP_FAKE_SERVER;

std::string WriteMcpsConfig(const nlohmann::json& entries) {
    static int counter = 0;
    const std::string path = "/tmp/minimal_mcp_session_test_" +
                             std::to_string(getpid()) + "_" + std::to_string(counter++) + ".json";
    std::ofstream(path) << entries.dump();
    return path;
}

nlohmann::json StdioEntry(const std::string& name, const std::string& mode) {
    return {{"name", name},
            {"transport", "stdio"},
            {"command", kFakeServer},
            {"args", {mode}},
            {"enabled", true}};
}

struct Fixture {
    explicit Fixture(const nlohmann::json& entries, const std::vector<std::string>& allowed)
        : mcp_config_path(WriteMcpsConfig(entries)) {
        preset.name = "test";
        preset.allowed_tools = {"*"};
        preset.allowed_mcp_servers = allowed;
        preset.permission_mode = testing::AllowAllPolicy();
        preset.run_mode = tools::PermissionRunMode::Normal;
    }

    void Start() {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();
        for (auto& step : scripted) owned_provider->PushResponse(step.first, step.second);
        auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
        store = owned_store.get();

        manager = std::make_unique<mcp::McpClientManager>(
            minimal::config::LoadMcpsConfig(mcp_config_path), registry, limits,
            minimal::common::DefaultLogger());

        agent::SessionParams params;
        params.session_id = "sess-mcp";
        params.agent_preset = "test";
        params.project_root = std::filesystem::temp_directory_path();
        params.mcp_manager = manager.get();
        rpc::FinishSessionWiring(params);

        session = std::make_unique<agent::Session>(
            std::move(params), preset, std::move(owned_provider), std::move(owned_store), registry,
            agent::LoopConfig{}, observers, minimal::common::DefaultLogger());
        stream = std::make_unique<testing::SessionStream>(*session);
    }

    void Script(llm::ChatResponse response) { scripted.emplace_back(std::move(response), std::vector<llm::StreamEvent>{}); }

    std::vector<v1::ServerEvent> DrainQueue() { return stream->Drain(); }

    ~Fixture() {
        stream.reset();
        if (session) session->Terminate();
        std::remove(mcp_config_path.c_str());
    }

    // The fixture's stand-in writer: subscribed for the fixture's lifetime so
    // DrainQueue never injects a resync notice into the event sequence.
    std::unique_ptr<testing::SessionStream> stream;

    std::string mcp_config_path;
    agent::AgentPreset preset;
    // Overridable per test: the cancel-wake test raises the request deadline
    // so the deadline cannot masquerade as the cancellation wake.
    mcp::McpLimits limits{std::chrono::seconds(3), std::chrono::seconds(5)};
    agent::SessionObserverRegistry observers{minimal::common::DefaultLogger()};
    tools::ToolRegistry registry;
    std::unique_ptr<mcp::McpClientManager> manager;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;
    std::unique_ptr<agent::Session> session;
    std::vector<std::pair<llm::ChatResponse, std::vector<llm::StreamEvent>>> scripted;
};

llm::ChatResponse TextResponse(const std::string& text) {
    llm::ChatResponse response;
    response.stop_reason = llm::StopReason::EndTurn;
    response.content.push_back(llm::TextBlock{text});
    return response;
}

}  // namespace

TEST_CASE("a live MCP server's tools reach the model and execute", "[session_mcp]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "normal")}), {"fake"});
    fixture.Script(TextResponse("done"));
    fixture.Start();
    REQUIRE(fixture.session->SubmitTurn("go"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto& requests = fixture.provider->requests();
    REQUIRE_FALSE(requests.empty());
    bool saw_echo = false;
    for (const auto& def : requests[0].tools) {
        if (def.name == "mcp__fake__echo") saw_echo = true;
    }
    CHECK(saw_echo);
}

TEST_CASE("a server that fails its liveness check is disabled with a surfaced error", "[session_mcp]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("broken", "garbage"),
                                           StdioEntry("fake", "normal")}),
                    {"broken", "fake"});
    fixture.Script(TextResponse("done"));
    fixture.Start();
    REQUIRE(fixture.session->SubmitTurn("go"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // The session STARTS (degradation, not abort) and an ErrorEvent names the
    // server -- the same channel a failed turn uses.
    bool surfaced = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_error() &&
            event.error().message().find("'broken'") != std::string::npos) {
            surfaced = true;
        }
    }
    CHECK(surfaced);

    // The dead server's tools never reach the model; the live one's do.
    const auto& requests = fixture.provider->requests();
    REQUIRE_FALSE(requests.empty());
    for (const auto& def : requests[0].tools) {
        INFO("tool: " << def.name);
        CHECK(def.name.rfind("mcp__broken__", 0) != 0);
    }
    bool saw_live = false;
    for (const auto& def : requests[0].tools) {
        if (def.name == "mcp__fake__echo") saw_live = true;
    }
    CHECK(saw_live);

    // The dead server is RE-CHECKED across an /agent-style re-resolve, not
    // permanently written off: a server that failed once used to stay
    // disabled for the session's life, and an /agent switch was the only way
    // back (and never took the chance). It is still "broken" (garbage mode
    // never recovers), so the retry itself still fails -- but the retry
    // attempt reaching EnsureAlive is exactly what a genuinely-recovered
    // server would also reach, and the error is still surfaced either way:
    // with the default restart_cooldown_ms, this second attempt lands inside
    // the cooldown window and answers with THAT error instead of the
    // original connect failure, which is itself proof the check re-ran
    // rather than being skipped by a sticky per-session blocklist.
    fixture.session->ApplyPreset(fixture.preset);
    surfaced = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_error() && event.error().message().find("'broken'") != std::string::npos) {
            surfaced = true;
        }
    }
    CHECK(surfaced);
}

TEST_CASE("a recovered server's tools return on an /agent-style re-resolve", "[session_mcp]") {
    // Unlike the "broken" (garbage-mode, permanently failing) case above,
    // this proves an actual RECOVERY: a server that dies after a healthy
    // start comes back once RefreshMcpServers re-attempts it, because
    // nothing marks it permanently disabled anymore.
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "die_after_ms:1500")}), {"fake"});
    fixture.limits.restart_cooldown = std::chrono::milliseconds(1);  // don't gate the retry below
    fixture.Script(TextResponse("done"));
    fixture.Script(TextResponse("done"));
    fixture.Start();

    REQUIRE(fixture.session->SubmitTurn("go"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    {
        const auto& requests = fixture.provider->requests();
        REQUIRE_FALSE(requests.empty());
        bool saw_echo = false;
        for (const auto& def : requests.back().tools) {
            if (def.name == "mcp__fake__echo") saw_echo = true;
        }
        CHECK(saw_echo);
    }

    std::this_thread::sleep_for(3s);  // outlive the scripted death

    // Not a fresh session, not a fresh Session::Start -- the same session,
    // re-resolving its MCP servers the way /agent does. The server is dead
    // right now; this must not be the permanent verdict.
    fixture.session->ApplyPreset(fixture.preset);
    REQUIRE(fixture.session->SubmitTurn("go again"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto& requests = fixture.provider->requests();
    REQUIRE(requests.size() >= 2);
    bool saw_echo_again = false;
    for (const auto& def : requests.back().tools) {
        if (def.name == "mcp__fake__echo") saw_echo_again = true;
    }
    CHECK(saw_echo_again);
}

TEST_CASE("de-scoping a server on /agent removes its tools without killing the process", "[session_mcp]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "normal")}), {"fake"});
    fixture.Script(TextResponse("first"));
    fixture.Script(TextResponse("second"));
    fixture.Start();
    REQUIRE(fixture.session->SubmitTurn("turn one"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    agent::AgentPreset rescoped = fixture.preset;
    rescoped.allowed_mcp_servers.clear();
    fixture.session->ApplyPreset(rescoped);

    fixture.session->SubmitTurn("turn two");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto& requests = fixture.provider->requests();
    REQUIRE(requests.size() == 2);
    // The first turn saw the tools; the second, after de-scoping, must not.
    bool saw_before = false;
    for (const auto& def : requests[0].tools) {
        if (def.name == "mcp__fake__echo") saw_before = true;
    }
    CHECK(saw_before);
    for (const auto& def : requests[1].tools) {
        INFO("tool: " << def.name);
        CHECK(def.name.rfind("mcp__fake__", 0) != 0);
    }
}

TEST_CASE("CancelTurn wakes a blocked MCP tool call", "[session_mcp]") {
    // The full event-driven cancellation chain: Session::Cancel fires the
    // turn's token, the token's cv wakes the parked McpConnection waiter, the
    // tool call degrades to a cancelled result, and the turn ends -- all well
    // inside the 30 s request deadline, which cannot be the wake-up source.
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "hang")}), {"fake"});
    fixture.limits = mcp::McpLimits{std::chrono::seconds(3), std::chrono::seconds(30)};
    fixture.Script(minimal::testing::ToolUseResponse("tu1", "mcp__fake__hang"));
    fixture.Start();
    REQUIRE(fixture.session->SubmitTurn("go"));

    // Reach the parked call: spawn (~0.5 s) + initialize + tools/list.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    fixture.session->Cancel();
    // Without the token wake this waits out the 30 s deadline; the idle
    // arrival here is the proof the wake worked.
    REQUIRE(fixture.session->WaitForIdleForTesting(std::chrono::seconds(6)));
}

TEST_CASE("a preset's disabled-but-configured server is auto-enabled at session start",
          "[session_mcp]") {
    nlohmann::json disabled = StdioEntry("fake", "normal");
    disabled["enabled"] = false;
    Fixture fixture(nlohmann::json::array({disabled}), {"fake"});
    fixture.Script(TextResponse("done"));
    fixture.Start();

    // RefreshMcpServers ran in the constructor, before any turn: the
    // disabled entry must already be live by the time the first turn's tool
    // list is built.
    CHECK(fixture.manager->StatusOf("fake").enabled);
    CHECK(fixture.manager->StatusOf("fake").alive);

    REQUIRE(fixture.session->SubmitTurn("go"));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto& requests = fixture.provider->requests();
    REQUIRE_FALSE(requests.empty());
    bool saw_echo = false;
    for (const auto& def : requests[0].tools) {
        if (def.name == "mcp__fake__echo") saw_echo = true;
    }
    CHECK(saw_echo);
}

TEST_CASE("a subagent whose preset requires a dead server does not start", "[session_mcp]") {
    namespace fs = std::filesystem;
    testing::TempDir dir;
    fs::create_directories(dir.path() / "agents");
    std::ofstream(dir.path() / "agents/main.md")
        << "---\nname: main\nallowed_tools: [\"*\"]\n"
           "allowed_mcp_servers: [\"broken\"]\n---\nmain\n";
    std::ofstream(dir.path() / "agents/child.md")
        << "---\nname: child\nallowed_tools: [\"*\"]\nallowed_mcp_servers: [\"broken\"]\n---\nchild\n";
    std::ofstream(dir.path() / "settings.json")
        << nlohmann::json{{"default_agent_preset", "main"},
                          {"default_model", "main/fake-model"}}.dump();
    std::ofstream(dir.path() / "permissions.json")
        << nlohmann::json{{"mcp_default_mode", "ask"}}.dump();
    std::ofstream(dir.path() / "providers.json")
        << nlohmann::json{{"providers",
                           {{"main",
                             {{"backend", "fake"},
                              {"url", "http://localhost/v1"},
                              {"models", {{"fake-model", nlohmann::json::object()}}}}}}}}
              .dump();

    // AgentPresetRegistry borrows the registry: it must outlive it, so a
    // local -- never a temporary.
    config::SkillRegistry skills = config::SkillRegistry::Load(dir.path());
    agent::AgentPresetRegistry presets(dir.path(), skills,
                                        config::LoadSettingsConfig(dir.path() / "settings.json"));
    agent::SessionObserverRegistry observers{minimal::common::DefaultLogger()};
    session::SessionDatabase db(dir.path() / "sessions.db");
    llm::ProviderFactoryRegistry providers;
    providers.Register("fake", [](const llm::ProviderConfig&)
                                   -> std::unique_ptr<llm::LlmProvider> {
        return std::make_unique<testing::FakeLlmProvider>();
    });
    tools::ToolRegistry tools;
    agent::SessionRegistry sessions(presets, tools, providers, observers, db, agent::LoopConfig{},
                                    minimal::common::DefaultLogger(), 60s);

    const std::string broken_config = WriteMcpsConfig(nlohmann::json::array({StdioEntry("broken", "garbage")}));
    mcp::McpClientManager manager(
        minimal::config::LoadMcpsConfig(broken_config), tools,
        mcp::McpLimits{std::chrono::seconds(2), std::chrono::seconds(2)},
        minimal::common::DefaultLogger());

    agent::SubagentManager subagents(sessions, db, presets, /*max_concurrent_subagents=*/4,
                                     minimal::common::DefaultLogger(), &manager);

    const auto row = db.CreateSession("", std::nullopt, dir.path().string(), "main");
    agent::SessionParams parent_params;
    parent_params.session_id = row.session_id;
    parent_params.agent_preset = "main";
    parent_params.project_root = dir.path();
    parent_params.mcp_manager = &manager;
    auto parent = sessions.Create(std::move(parent_params));
    // a client is present in every designed flow
    const auto parent_sub = testing::Transport(*parent).queue().Subscribe();
    parent->NotifyAttached();

    agent::SubagentRequest request;
    request.task = "do the thing";
    request.agent_preset = "child";
    request.blocking = false;
    tools::ToolContext ctx{minimal::common::DefaultLogger()};
    ctx.session_id = parent->info().session_id;
    const agent::SubagentOutcome outcome = subagents.Spawn(request, ctx);
    REQUIRE(outcome.kind == agent::SubagentOutcome::Kind::kError);
    CHECK(outcome.error.find("unavailable") != std::string::npos);
    CHECK(outcome.child_session_id.empty());

    std::remove(broken_config.c_str());
}
