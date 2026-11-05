// The tool-ask plugin's tool: input validation, the answer's per-slot
// validation (the plugin's own, since core carries the answer opaquely),
// per-outcome results, and answer formatting. The prompt broker is a lambda
// in the ToolContext -- exactly the seam the tool consumes -- so the real
// round-trip is pinned down by wire_plugin_prompt_broker_test.cpp.

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ask_tool.hpp"

using namespace minimal;

namespace {

tools::ToolContext Context() { return tools::ToolContext{common::DefaultLogger()}; }

tools::ToolContext ContextWithBroker(
    std::function<tools::PromptResult(const tools::PromptRequest&)> broker) {
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.prompt_user = std::move(broker);
    return ctx;
}

// A broker that answers with these per-question slots.
// A vector, not a json: `json answers = {Slot(...)}` with ONE element is a
// copy of the slot under some compilers (AppleClang 17) and a one-element
// array under others -- the single-answer cases read as "dismissed" on CI.
tools::ToolContext Answering(const std::vector<nlohmann::json>& slots) {
    const nlohmann::json answers(slots);
    return ContextWithBroker([answers](const tools::PromptRequest&) {
        return tools::PromptResult{tools::PromptOutcome::Answered,
                                   nlohmann::json{{"answers", answers}}};
    });
}

nlohmann::json Slot(std::vector<std::string> selected, std::string other = {}) {
    return nlohmann::json{{"selected", selected}, {"other", other}};
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

TEST_CASE("ask_user reports unavailable, not an error, when no prompt broker is wired",
          "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = Context();
    const auto result = tool->execute(OneQuestion(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("not available") != std::string::npos);
    CHECK(result.display.at("outcome") == "unavailable");
}

TEST_CASE("the prompt is kind \"ask\" carrying the validated request", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    std::optional<tools::PromptRequest> seen;
    tools::ToolContext ctx = ContextWithBroker([&seen](const tools::PromptRequest& request) {
        seen = request;
        return tools::PromptResult{};
    });
    tool->execute(OneQuestion(), ctx);
    REQUIRE(seen.has_value());
    CHECK(seen->kind == "ask");
    const auto& question = seen->payload.at("questions").at(0);
    CHECK(question.at("question") == "Which approach?");
    CHECK(question.at("header") == "Approach");
    CHECK(question.at("multiple") == false);
    CHECK(question.at("allow_other") == true);
    CHECK(question.at("options").at(1).at("label") == "patch");
    CHECK(question.at("options").at(1).at("description") == "");
}

TEST_CASE("each outcome reads differently to the model", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    const auto run = [&tool](tools::PromptOutcome outcome) {
        tools::ToolContext ctx = ContextWithBroker(
            [outcome](const tools::PromptRequest&) { return tools::PromptResult{outcome, nullptr}; });
        return tool->execute(OneQuestion(), ctx);
    };
    const auto dismissed = run(tools::PromptOutcome::Dismissed);
    const auto timed_out = run(tools::PromptOutcome::TimedOut);
    const auto cancelled = run(tools::PromptOutcome::Cancelled);
    const auto unavailable = run(tools::PromptOutcome::Unavailable);
    for (const auto* result : {&dismissed, &timed_out, &cancelled, &unavailable}) {
        CHECK_FALSE(result->is_error);   // an error would invite a retry
    }
    CHECK(dismissed.content_text.find("dismissed") != std::string::npos);
    CHECK(timed_out.content_text.find("timeout") != std::string::npos);
    CHECK(timed_out.content_text.find("dismissed") == std::string::npos);
    CHECK(cancelled.content_text.find("cancelled") != std::string::npos);
    CHECK(unavailable.content_text.find("not available") != std::string::npos);
    CHECK(timed_out.display.at("outcome") == "timed_out");
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
    SECTION("duplicate option labels") {
        nlohmann::json input = OneQuestion();
        input["questions"][0]["options"][1]["label"] = "rewrite";
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("unique") != std::string::npos);
    }
    SECTION("a wrongly typed optional field is an error, not silently dropped") {
        nlohmann::json input = OneQuestion();
        SECTION("header") { input["questions"][0]["header"] = 7; }
        SECTION("description") { input["questions"][0]["options"][0]["description"] = false; }
        SECTION("multiple") { input["questions"][0]["multiple"] = "yes"; }
        SECTION("allow_other") { input["questions"][0]["allow_other"] = 1; }
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
    }
    SECTION("over-long strings") {
        nlohmann::json input = OneQuestion();
        SECTION("header") { input["questions"][0]["header"] = std::string(41, 'h'); }
        SECTION("question") { input["questions"][0]["question"] = std::string(1001, 'q'); }
        SECTION("label") { input["questions"][0]["options"][0]["label"] = std::string(201, 'l'); }
        SECTION("description") {
            input["questions"][0]["options"][0]["description"] = std::string(501, 'd');
        }
        const auto result = tool->execute(input, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("longer than") != std::string::npos);
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
        tools::ToolContext ctx = Answering({Slot({"patch"})});
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "1. Approach: the user selected: patch");
    }

    SECTION("free text reads as the user's answer") {
        tools::ToolContext ctx = Answering({Slot({}, "use a config flag")});
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "1. Approach: the user answered: \"use a config flag\"");
    }

    SECTION("a question left blank says so instead of inventing an answer") {
        nlohmann::json input = OneQuestion();
        input["questions"].push_back(input["questions"][0]);
        input["questions"][1]["header"] = "Scope";
        tools::ToolContext ctx = Answering({Slot({"patch"}), Slot({})});
        const auto result = tool->execute(input, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text ==
              "1. Approach: the user selected: patch\n2. Scope: the user did not answer");
    }

    SECTION("free text with a newline stays on one line") {
        tools::ToolContext ctx = Answering({Slot({}, "line one\nline two")});
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK(result.content_text == "1. Approach: the user answered: \"line one\\nline two\"");
    }

    SECTION("submitting with nothing picked anywhere reads as a dismissal") {
        tools::ToolContext ctx = Answering({Slot({})});
        const auto result = tool->execute(OneQuestion(), ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text.find("dismissed") != std::string::npos);
    }

    SECTION("multi-select lists every label") {
        nlohmann::json input = OneQuestion();
        input["questions"][0]["multiple"] = true;
        tools::ToolContext ctx = Answering({Slot({"rewrite", "patch"})});
        const auto result = tool->execute(input, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "1. Approach: the user selected: rewrite, patch");
    }

    SECTION("a question without a header is named by its own text") {
        nlohmann::json input = OneQuestion();
        input["questions"][0].erase("header");
        tools::ToolContext ctx = Answering({Slot({"patch"})});
        const auto result = tool->execute(input, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text == "1. \"Which approach?\": the user selected: patch");
    }
}

TEST_CASE("a dismissed ask is a plain result the model adapts to, not an error",
          "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = ContextWithBroker([](const tools::PromptRequest&) {
        return tools::PromptResult{tools::PromptOutcome::Dismissed, nullptr};
    });

    const auto result = tool->execute(OneQuestion(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("dismissed") != std::string::npos);
}

TEST_CASE("allow_other reaches the request; it defaults to true", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    std::optional<bool> seen;
    tools::ToolContext ctx = ContextWithBroker([&seen](const tools::PromptRequest& request) {
        seen = request.payload.at("questions").at(0).at("allow_other").get<bool>();
        return tools::PromptResult{};
    });
    nlohmann::json input = OneQuestion();
    seen.reset();
    tool->execute(input, ctx);
    CHECK(seen == true);
    input["questions"][0]["allow_other"] = false;
    seen.reset();
    tool->execute(input, ctx);
    CHECK(seen == false);
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
    CHECK(question_item.at("properties").contains("allow_other"));

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

// The answer arrives from a client, so the tool reads it fail-closed per slot:
// anything the question did not offer, or a shape its flags forbid, is "the
// user did not answer" -- never an invented label.
TEST_CASE("an answer slot the question does not allow is read as unanswered", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    nlohmann::json input = OneQuestion();
    input["questions"].push_back(input["questions"][0]);
    input["questions"][1]["header"] = "Scope";

    nlohmann::json bad;
    SECTION("a label never offered") { bad = Slot({"maybe"}); }
    SECTION("two picks on a single-select question") { bad = Slot({"rewrite", "patch"}); }
    SECTION("free text mixed with a selection") { bad = Slot({"patch"}, "plus a note"); }
    SECTION("wrong JSON types") { bad = nlohmann::json{{"selected", "patch"}}; }
    SECTION("free text where allow_other is false") {
        input["questions"][1]["allow_other"] = false;
        bad = Slot({}, "neither");
    }
    tools::ToolContext ctx = Answering({Slot({"patch"}), bad});
    const auto result = tool->execute(input, ctx);
    CHECK(result.content_text ==
          "1. Approach: the user selected: patch\n2. Scope: the user did not answer");
}

TEST_CASE("multi-select accepts several offered picks, not a repeated one", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    nlohmann::json input = OneQuestion();
    input["questions"][0]["multiple"] = true;
    {
        tools::ToolContext ctx = Answering({Slot({"patch", "rewrite"})});
        CHECK(tool->execute(input, ctx).content_text ==
              "1. Approach: the user selected: patch, rewrite");
    }
    {
        tools::ToolContext ctx = Answering({Slot({"patch", "patch"})});
        CHECK(tool->execute(input, ctx).content_text.find("dismissed") != std::string::npos);
    }
}

TEST_CASE("the display payload records every question and its answer", "[tool_ask]") {
    auto tool = tool_ask::MakeAskTool();
    tools::ToolContext ctx = Answering({Slot({}, "my own")});
    const auto result = tool->execute(OneQuestion(), ctx);
    REQUIRE(result.display.is_object());
    CHECK(result.display.at("kind") == "ask");
    CHECK(result.display.at("outcome") == "answered");
    const auto& question = result.display.at("questions").at(0);
    CHECK(question.at("header") == "Approach");
    CHECK(question.at("question") == "Which approach?");
    CHECK(question.at("other") == "my own");
    CHECK(question.at("selected").empty());
}
