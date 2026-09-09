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
    return nlohmann::json{
        {"questions",
         nlohmann::json::array({nlohmann::json{
             {"question", "Which approach?"},
             {"header", "Approach"},
             {"options", nlohmann::json::array(
                             {nlohmann::json{{"label", "rewrite"}, {"description", "Start over"}},
                              nlohmann::json{{"label", "patch"}, {"description", "Small fix"}}})},
         }})}};
}

}  // namespace

TEST_CASE("question degrades to an error result when no ask broker is wired", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = Context();
    const auto result = tool->execute(OneQuestion(), ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("not available") != std::string::npos);
}

TEST_CASE("question enforces its input schema before asking anyone anything", "[tool_ask]") {
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
                {"question", "q"},
                {"header", "h"},
                {"options", nlohmann::json::array(
                                {nlohmann::json{{"label", "a"}, {"description", "d"}}})}});
        }
        const auto result = tool->execute(nlohmann::json{{"questions", questions}}, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("at most 4") != std::string::npos);
    }
    SECTION("missing header") {
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array({nlohmann::json{
                 {"question", "q"},
                 {"options", nlohmann::json::array(
                                 {nlohmann::json{{"label", "a"}, {"description", "d"}}})}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("header") != std::string::npos);
    }
    SECTION("option without a label") {
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array({nlohmann::json{
                 {"question", "q"},
                 {"header", "h"},
                 {"options", nlohmann::json::array({nlohmann::json{
                                 {"description", "no label"}}})}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
    }
    SECTION("option without a description") {
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array({nlohmann::json{
                 {"question", "q"},
                 {"header", "h"},
                 {"options",
                  nlohmann::json::array({nlohmann::json{{"label", "no description"}}})}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("description") != std::string::npos);
    }
    SECTION("too many options") {
        nlohmann::json options = nlohmann::json::array();
        for (int i = 0; i < 6; ++i) options.push_back(nlohmann::json{{"label", "x"}});
        const nlohmann::json input = nlohmann::json{
            {"questions",
             nlohmann::json::array(
                 {nlohmann::json{{"question", "q"}, {"header", "h"}, {"options", options}}})}};
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("at most 5") != std::string::npos);
    }
}

TEST_CASE("question formats selections and free text as prose", "[tool_ask]") {
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

TEST_CASE("the question tool is ReadOnly and never parallel-safe", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    CHECK(tool->spec().category == tools::ToolCategory::ReadOnly);
    CHECK_FALSE(tool->parallel_safe());
}
