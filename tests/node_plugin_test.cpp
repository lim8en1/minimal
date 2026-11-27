// The node plugin (plugins/server/node) and the core seams it is the first
// consumer of: ToolResult::turn_result, preset tool_choice, turn gates.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "session_events.hpp"
#include "minimal/agent/preset_extensions.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/turn_gate.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/session_wiring.hpp"

#include "../plugins/server/node/input_gate.hpp"
#include "../plugins/server/node/node_config.hpp"
#include "../plugins/server/node/schema_cache.hpp"
#include "../plugins/server/node/submit_result_tool.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

nlohmann::json OutputSchema() {
    return nlohmann::json::parse(R"({
        "type": "object",
        "required": ["total", "currency"],
        "additionalProperties": false,
        "properties": {
            "total": {"type": "number"},
            "currency": {"type": "string", "enum": ["EUR", "USD"]}
        }
    })");
}

nlohmann::json InputSchema() {
    return nlohmann::json::parse(R"({
        "type": "object",
        "required": ["text"],
        "properties": {"text": {"type": "string"}}
    })");
}

agent::AgentPreset NodePreset(bool with_input = true, bool with_output = true) {
    agent::AgentPreset preset;
    preset.name = "node";
    preset.allowed_tools = {"*"};
    preset.permission_mode = testing::AllowAllPolicy();
    preset.run_mode = tools::PermissionRunMode::Normal;
    preset.tool_choice_required = true;
    nlohmann::json node = nlohmann::json::object();
    if (with_input) node["input_schema"] = InputSchema();
    if (with_output) node["output_schema"] = OutputSchema();
    preset.tool_config["node"] = node;
    return preset;
}

llm::ToolDefinition Build(const agent::AgentPreset& preset, node::SchemaCache& schemas) {
    const std::string key = "node-key";
    const agent::PresetToolEnv env;
    const std::filesystem::path root;
    return node::BuildSubmitResultDefinition(
        agent::PresetToolContext{preset, key, env, root, nullptr}, schemas);
}

tools::ToolResult Submit(tools::Tool& tool, const agent::AgentPreset& preset,
                         const nlohmann::json& input) {
    tools::ToolContext ctx(common::DefaultLogger());
    if (auto it = preset.tool_config.find("node"); it != preset.tool_config.end()) {
        ctx.tool_config = &it.value();
    }
    return tool.execute(input, ctx);
}

std::optional<std::string> Gate(agent::TurnGate& gate, const agent::AgentPreset& preset,
                                const std::string& prompt) {
    return gate.Check(agent::TurnGateRequest{"s", preset, prompt});
}

}  // namespace

// --- schema cache and strictness -------------------------------------------

TEST_CASE("node: schemas compile once and report every violation with its path", "[node]") {
    node::SchemaCache schemas;
    const auto compiled = schemas.Get(OutputSchema());
    CHECK(schemas.Get(OutputSchema()) == compiled);

    CHECK(compiled->Validate(nlohmann::json{{"total", 12.5}, {"currency", "EUR"}}).empty());
    const auto violations =
        compiled->Validate(nlohmann::json{{"total", "12"}, {"currency", "GBP"}, {"x", 1}});
    CHECK(violations.size() >= 2);
    const std::string text = node::FormatViolations(violations);
    CHECK(text.find("/total") != std::string::npos);
    CHECK(text.find("/currency") != std::string::npos);
}

TEST_CASE("node: an invalid schema does not compile", "[node]") {
    node::SchemaCache schemas;
    CHECK_THROWS_AS(schemas.Get(nlohmann::json{{"type", "no-such-type"}}), std::invalid_argument);
    CHECK_THROWS_AS(schemas.Get(nlohmann::json{{"$ref", "https://example.com/s.json"}}),
                    std::invalid_argument);
}

TEST_CASE("node: strict compatibility needs every object closed and fully required", "[node]") {
    CHECK(node::IsStrictCompatible(OutputSchema()));
    CHECK_FALSE(node::IsStrictCompatible(InputSchema()));  // additionalProperties open

    auto partial = OutputSchema();
    partial["required"] = nlohmann::json::array({"total"});
    CHECK_FALSE(node::IsStrictCompatible(partial));

    auto nested = OutputSchema();
    nested["properties"]["lines"] = nlohmann::json::parse(
        R"({"type": "array", "items": {"type": "object", "properties": {"a": {"type": "string"}}}})");
    nested["required"].push_back("lines");
    CHECK_FALSE(node::IsStrictCompatible(nested));
    nested["properties"]["lines"]["items"]["required"] = nlohmann::json::array({"a"});
    nested["properties"]["lines"]["items"]["additionalProperties"] = false;
    CHECK(node::IsStrictCompatible(nested));
}

// --- the node slice ----------------------------------------------------------

TEST_CASE("node: the tool_config slice rejects unknown keys and non-object schemas", "[node]") {
    CHECK_NOTHROW(node::ParseNodeConfig(nlohmann::json::object()));
    CHECK_THROWS_AS(node::ParseNodeConfig(nlohmann::json{{"output", OutputSchema()}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(node::ParseNodeConfig(nlohmann::json{{"output_schema", "object"}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(node::ParseNodeConfig(nlohmann::json::array()), std::invalid_argument);
}

// --- submit_result: the per-preset definition ------------------------------

TEST_CASE("node: submit_result advertises the preset's output schema, strict when it can be",
          "[node]") {
    node::SchemaCache schemas;
    const auto definition = Build(NodePreset(), schemas);
    CHECK(definition.name == "submit_result");
    CHECK(definition.input_schema == OutputSchema());
    CHECK(definition.strict);
}

TEST_CASE("node: submit_result is withheld from presets without an output schema", "[node]") {
    node::SchemaCache schemas;
    agent::AgentPreset plain;
    CHECK(Build(plain, schemas).name.empty());
    CHECK(Build(NodePreset(/*with_input=*/true, /*with_output=*/false), schemas).name.empty());
}

TEST_CASE("node: a broken contract fails the preset's resolution", "[node]") {
    node::SchemaCache schemas;
    auto bad_output = NodePreset();
    bad_output.tool_config["node"]["output_schema"] = nlohmann::json{{"type", 7}};
    CHECK_THROWS_AS(Build(bad_output, schemas), std::invalid_argument);

    auto bad_input = NodePreset();
    bad_input.tool_config["node"]["input_schema"] = nlohmann::json{{"type", "nope"}};
    CHECK_THROWS_AS(Build(bad_input, schemas), std::invalid_argument);

    auto typo = NodePreset();
    typo.tool_config["node"]["ouput_schema"] = OutputSchema();
    CHECK_THROWS_AS(Build(typo, schemas), std::invalid_argument);
}

// --- submit_result: execution -------------------------------------------------

TEST_CASE("node: a valid submission is accepted as the turn result", "[node]") {
    auto tool = node::MakeSubmitResultTool(std::make_shared<node::SchemaCache>());
    const auto input = nlohmann::json{{"total", 12.5}, {"currency", "EUR"}};
    const auto result = Submit(*tool, NodePreset(), input);
    CHECK_FALSE(result.is_error);
    REQUIRE(result.turn_result.has_value());
    CHECK(*result.turn_result == input);
}

TEST_CASE("node: an invalid submission returns the violations and no result", "[node]") {
    auto tool = node::MakeSubmitResultTool(std::make_shared<node::SchemaCache>());
    const auto result = Submit(*tool, NodePreset(), nlohmann::json{{"total", 12.5}});
    CHECK(result.is_error);
    CHECK_FALSE(result.turn_result.has_value());
    CHECK(result.content_text.find("currency") != std::string::npos);
}

TEST_CASE("node: submit_result refuses when the active preset declares no output schema",
          "[node]") {
    auto tool = node::MakeSubmitResultTool(std::make_shared<node::SchemaCache>());
    const auto result = Submit(*tool, agent::AgentPreset{}, nlohmann::json::object());
    CHECK(result.is_error);
    CHECK_FALSE(result.turn_result.has_value());
}

// --- the input gate -------------------------------------------------------------

TEST_CASE("node: the input gate checks a node's prompt against its input schema", "[node]") {
    auto gate = node::MakeInputGate(std::make_shared<node::SchemaCache>());
    const auto preset = NodePreset();
    CHECK_FALSE(Gate(*gate, preset, R"({"text": "Total 12.50 EUR"})").has_value());

    const auto not_json = Gate(*gate, preset, "please extract the invoice");
    REQUIRE(not_json.has_value());
    CHECK(not_json->find("not JSON") != std::string::npos);

    const auto wrong = Gate(*gate, preset, R"({"text": 12})");
    REQUIRE(wrong.has_value());
    CHECK(wrong->find("/text") != std::string::npos);
}

TEST_CASE("node: the input gate passes anything for presets without an input schema", "[node]") {
    auto gate = node::MakeInputGate(std::make_shared<node::SchemaCache>());
    CHECK_FALSE(Gate(*gate, agent::AgentPreset{}, "free text").has_value());
    CHECK_FALSE(
        Gate(*gate, NodePreset(/*with_input=*/false), "free text").has_value());
}

// --- the turn gate registry ---------------------------------------------------

TEST_CASE("turn gates: first rejection wins, and a throwing gate fails closed", "[turn_gate]") {
    struct Scripted : agent::TurnGate {
        std::optional<std::string> verdict;
        bool throws = false;
        int asked = 0;
        std::optional<std::string> Check(const agent::TurnGateRequest&) override {
            ++asked;
            if (throws) throw std::runtime_error("boom");
            return verdict;
        }
    };
    agent::TurnGateRegistry registry(common::DefaultLogger());
    auto pass = std::make_shared<Scripted>();
    auto reject = std::make_shared<Scripted>();
    reject->verdict = "no";
    auto later = std::make_shared<Scripted>();
    registry.SetAttribution("p1");
    registry.Register(pass);
    registry.Register(reject);
    registry.Register(later);
    registry.ClearAttribution();

    const agent::AgentPreset preset;
    const std::string prompt = "x";
    CHECK(registry.Check({"s", preset, prompt}) == std::optional<std::string>("no"));
    CHECK(later->asked == 0);

    agent::TurnGateRegistry throwing(common::DefaultLogger());
    auto broken = std::make_shared<Scripted>();
    broken->throws = true;
    throwing.SetAttribution("broken-plugin");
    throwing.Register(broken);
    throwing.ClearAttribution();
    const auto verdict = throwing.Check({"s", preset, prompt});
    REQUIRE(verdict.has_value());
    CHECK(verdict->find("broken-plugin") != std::string::npos);

    registry.RemoveByAttribution("p1");
    CHECK(registry.CountOwnedBy("p1") == 0);
    CHECK_FALSE(registry.Check({"s", preset, prompt}).has_value());
}

// --- end to end over a session ------------------------------------------------

namespace {

// A session wired the way the registry funnel wires one: the node tool in the
// registry, the node gate in LoopConfig::turn_gates, the preset carrying the
// definition override the builder would have produced.
struct NodeSessionFixture {
    NodeSessionFixture() : observers(common::DefaultLogger()), gates(common::DefaultLogger()) {
        auto schemas = std::make_shared<node::SchemaCache>();
        tools_registry.Register(node::MakeSubmitResultTool(schemas));
        gates.Register(node::MakeInputGate(schemas));
        preset = NodePreset();
        preset.tool_definition_overrides["submit_result"] = Build(preset, *schemas);
    }

    void Start() {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();
        for (auto& response : scripted) owned_provider->PushResponse(response);
        auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
        store = owned_store.get();

        agent::SessionParams params;
        params.session_id = "sess-node";
        params.name = "node session";
        params.agent_preset = "node";
        params.project_root = std::filesystem::temp_directory_path();
        loop_config.turn_gates = &gates;
        rpc::FinishSessionWiring(params);

        session = std::make_unique<agent::Session>(
            std::move(params), preset, std::move(owned_provider), std::move(owned_store),
            tools_registry, loop_config, observers, common::DefaultLogger());
        stream = std::make_unique<testing::SessionStream>(*session);
    }

    std::optional<v1::TurnCompletedEvent> RunTurn(const std::string& prompt) {
        REQUIRE(session->SubmitTurn(prompt));
        REQUIRE(session->WaitForIdleForTesting(5s));
        for (const auto& event : stream->Drain()) {
            if (event.has_turn_completed()) return event.turn_completed();
        }
        return std::nullopt;
    }

    ~NodeSessionFixture() {
        stream.reset();
        if (session) session->Terminate();
    }

    agent::SessionObserverRegistry observers;
    agent::TurnGateRegistry gates;
    agent::AgentPreset preset;
    agent::LoopConfig loop_config;
    tools::ToolRegistry tools_registry;
    std::vector<llm::ChatResponse> scripted;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;
    std::unique_ptr<agent::Session> session;
    std::unique_ptr<testing::SessionStream> stream;
};

}  // namespace

TEST_CASE("node e2e: an invalid submission is retried, the valid one is the wire result",
          "[node][session]") {
    NodeSessionFixture f;
    f.scripted.push_back(testing::ToolUseResponse("t1", "submit_result", {{"total", "12.50"}}));
    f.scripted.push_back(
        testing::ToolUseResponse("t2", "submit_result", {{"total", 12.5}, {"currency", "EUR"}}));
    f.Start();

    const auto completed = f.RunTurn(R"({"text": "Total 12.50 EUR"})");

    REQUIRE(completed.has_value());
    CHECK(completed->stop_reason() == v1::STOP_REASON_END_TURN);
    CHECK(nlohmann::json::parse(completed->result_json()) ==
          nlohmann::json{{"total", 12.5}, {"currency", "EUR"}});
    CHECK(f.provider->call_count() == 2);
    // Every request demanded a tool call and offered the contract as the
    // tool's schema.
    for (const auto& request : f.provider->requests()) {
        CHECK(request.tool_choice.kind == llm::ToolChoiceKind::Required);
        const auto it = std::find_if(request.tools.begin(), request.tools.end(),
                                     [](const auto& tool) { return tool.name == "submit_result"; });
        REQUIRE(it != request.tools.end());
        CHECK(it->input_schema == OutputSchema());
    }
}

TEST_CASE("node e2e: input violating the contract is rejected before any provider call",
          "[node][session]") {
    NodeSessionFixture f;
    f.Start();

    const auto completed = f.RunTurn(R"({"txt": "Total 12.50 EUR"})");

    REQUIRE(completed.has_value());
    CHECK(completed->stop_reason() == v1::STOP_REASON_REJECTED);
    CHECK(completed->result_json().empty());
    CHECK(f.provider->call_count() == 0);
    CHECK(f.store->messages.empty());
}
