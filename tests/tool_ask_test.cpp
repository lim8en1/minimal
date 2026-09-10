// The tool-ask plugin's tool: input validation, degrade-on-no-broker, and
// answer formatting. The broker itself is a lambda in the ToolContext --
// exactly the seam the tool consumes -- so the real round-trip behavior is
// pinned down by wire_ask_broker_test.cpp, not duplicated here.

#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ask_tool.hpp"

using namespace minimal;

namespace {

tools::ToolContext Context() { return tools::ToolContext{common::DefaultLogger()}; }

tools::ToolContext ContextWithBroker(tools::AskAnswer (*broker)(const tools::AskRequest&)) {
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.ask_user = broker;
    return ctx;
}

nlohmann::json OneQuestion() {
    return nlohmann::json{{"questions",
                           nlohmann::json::array({nlohmann::json{
                               {"question", "Which approach?"},
                               {"header", "Approach"},
                               {"options",
                                nlohmann::json::array({nlohmann::json{{"label", "rewrite"}},
                                                       nlohmann::json{{"label", "patch"}}})},
                           }})}};
}

}  // namespace

TEST_CASE("ask_user degrades to an error result when no ask broker is wired", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = Context();
    const auto result = tool->execute(OneQuestion(), ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("not available") != std::string::npos);
}

TEST_CASE("ask_user enforces its input schema before asking anyone anything", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    // No broker: validation errors must fire BEFORE the (absent) round-trip.
    tools::ToolContext ctx = Context();

    SECTION("missing questions") {
        const auto result = tool->execute(nlohmann::json{}, ctx);
        CHECK(result.is_error);
    }
    SECTION("empty questions") {
        const auto result = tool->execute(
            nlohmann::json{{"questions", nlohmann::json::array()}}, ctx);
        CHECK(result.is_error);
    }
    SECTION("too many questions") {
        nlohmann::json questions = nlohmann::json::array();
        for (int i = 0; i < 5; ++i) {
            questions.push_back(nlohmann::json{
                {"question", "q"}, {"options", nlohmann::json::array({nlohmann::json{{"label", "a"}}})}});
        }
        const auto result = tool->execute(nlohmann::json{{"questions", questions}}, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("at most 4") != std::string::npos);
    }
    SECTION("option without a label") {
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array({nlohmann::json{
                 {"question", "q"},
                 {"options", nlohmann::json::array({nlohmann::json{
                                 {"description", "no label"}}})}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
    }
    SECTION("too many options") {
        nlohmann::json options = nlohmann::json::array();
        for (int i = 0; i < 6; ++i) options.push_back(nlohmann::json{{"label", "x"}});
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array({nlohmann::json{{"question", "q"}, {"options", options}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("at most 5") != std::string::npos);
    }
}

TEST_CASE("ask_user formats selections and free text as prose", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();

    SECTION("a selected label is echoed verbatim") {
        tools::ToolContext ctx = ContextWithBroker([](const tools::AskRequest& request) {
            tools::AskAnswer answer;
            answer.answered = true;
            answer.slots.push_back({{"patch"}, {}});
            return answer;
        });
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "Approach: the user selected: patch");
    }

    SECTION("free text reads as the user's answer") {
        tools::ToolContext ctx = ContextWithBroker([](const tools::AskRequest&) {
            tools::AskAnswer answer;
            answer.answered = true;
            answer.slots.push_back(tools::AskAnswer::Slot{{}, "use a config flag"});
            return answer;
        });
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "Approach: the user answered: use a config flag");
    }

    SECTION("a question left blank says so instead of inventing an answer") {
        tools::ToolContext ctx = ContextWithBroker([](const tools::AskRequest&) {
            tools::AskAnswer answer;
            answer.answered = true;
            answer.slots.push_back(tools::AskAnswer::Slot{});
            return answer;
        });
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "Approach: the user did not answer");
    }

    SECTION("multi-select lists every label") {
        tools::ToolContext ctx = ContextWithBroker([](const tools::AskRequest&) {
            tools::AskAnswer answer;
            answer.answered = true;
            answer.slots.push_back(tools::AskAnswer::Slot{{"rewrite", "patch"}, {}});
            return answer;
        });
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "Approach: the user selected: rewrite, patch");
    }

    SECTION("a question without a header falls back to its index") {
        nlohmann::json input = OneQuestion();
        input["questions"][0].erase("header");
        tools::ToolContext ctx = ContextWithBroker([](const tools::AskRequest&) {
            tools::AskAnswer answer;
            answer.answered = true;
            answer.slots.push_back({{"patch"}, {}});
            return answer;
        });
        const auto result = tool->execute(input, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "Question 1: the user selected: patch");
    }
}

TEST_CASE("a dismissed ask is a plain result the model adapts to, not an error",
          "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = ContextWithBroker(
        [](const tools::AskRequest&) { return tools::AskAnswer{}; });  // answered=false

    const auto result = tool->execute(OneQuestion(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("dismissed") != std::string::npos);
}

TEST_CASE("the ask tool is ReadOnly and never parallel-safe", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    CHECK(tool->spec().category == tools::ToolCategory::ReadOnly);
    CHECK_FALSE(tool->parallel_safe());
}

// nlohmann::json's brace-init constructor silently degrades a whole level to
// a JSON ARRAY the instant one "pair" in that level has more than 2
// elements -- an easy nested-brace mistake that produces a schema which
// reads fine in the source but is structurally invalid on the wire, with no
// compile error and nothing else in this file (which only ever exercises
// ParseQuestions on hand-built JSON, never the advertised schema) to catch
// it. Every object level the model actually sees is asserted here.
TEST_CASE("the advertised input schema is well-formed at every object level", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    const auto& schema = tool->spec().definition.input_schema;

    REQUIRE(schema.is_object());
    CHECK(schema.at("type") == "object");
    CHECK(schema.at("required") == nlohmann::json::array({"questions"}));

    const auto& questions = schema.at("properties").at("questions");
    REQUIRE(questions.is_object());
    CHECK(questions.at("type") == "array");

    const auto& question_item = questions.at("items");
    REQUIRE(question_item.is_object());
    CHECK(question_item.at("type") == "object");
    CHECK(question_item.at("required") == nlohmann::json::array({"question", "options"}));
    REQUIRE(question_item.at("properties").is_object());
    CHECK(question_item.at("properties").contains("question"));
    CHECK(question_item.at("properties").contains("header"));
    CHECK(question_item.at("properties").contains("multiple"));

    const auto& options = question_item.at("properties").at("options");
    REQUIRE(options.is_object());
    CHECK(options.at("type") == "array");

    const auto& option_item = options.at("items");
    REQUIRE(option_item.is_object());
    CHECK(option_item.at("type") == "object");
    CHECK(option_item.at("required") == nlohmann::json::array({"label"}));
    REQUIRE(option_item.at("properties").is_object());
    CHECK(option_item.at("properties").contains("label"));
    CHECK(option_item.at("properties").contains("description"));
}
