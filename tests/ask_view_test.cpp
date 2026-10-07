// The ask-view client plugin: the "ask" prompt renderer (the modal ask_user
// parks on) and the "ask" display renderer (its transcript row). The plugin's
// sources compile straight into the test binary, like the other bundled
// client plugins' pure parts.
//
// Navigation notes (ftxui semantics, worth writing down because the tests are
// the contract): a radiobox CONSUMES arrows to move its highlight and commits
// on space; the modal commits on Enter too (the radiobox never sees Enter).
// An ArrowDown past a list's last row bubbles out to the next widget.

#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "ask_display.hpp"
#include "ask_modal.hpp"
#include "md4c_engine.hpp"
#include "chat_renderers_fixture.hpp"
#include "ftxui_harness.hpp"
#include "widget_kit_fixture.hpp"
#include "minimal/client/sdk/chat_model.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/state.hpp"
#include "chat_view_fixture.hpp"

using namespace minimal::client;
using namespace minimal_test;
using minimal::ask_view::AskQuestion;
using minimal::ask_view::MakeAskModal;
namespace ftx = ftxui;
namespace v1 = minimal::v1;

namespace {

Theme TestTheme() { return Theme::Parse(R"({"name":"test","colors":{}})", ColorDepth::Truecolor); }

AskQuestion Q(const std::string& header, const std::string& question,
              const std::vector<std::string>& labels, bool multiple = false) {
    AskQuestion q;
    q.header = header;
    q.question = question;
    q.multiple = multiple;
    for (const auto& label : labels) q.options.push_back({label, ""});
    return q;
}

// Every reply the modal sent.
struct Replies {
    std::vector<plugins::PromptReply> sent;
    std::function<void(plugins::PromptReply)> sink() {
        return [this](plugins::PromptReply r) { sent.push_back(std::move(r)); };
    }
    const nlohmann::json& slot(std::size_t i) const {
        return sent.back().answer.at("answers").at(i);
    }
};

}  // namespace

// --- rendering ---------------------------------------------------------------

TEST_CASE("ask modal: every question renders, numbered when there are several",
          "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"}),
                               Q("Scope", "how much scope?", {"small", "large"})},
                              "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("1/2  Approach") != std::string::npos);
    CHECK(frame.find("which approach?") != std::string::npos);
    CHECK(frame.find("2/2  Scope") != std::string::npos);
    CHECK(frame.find("how much scope?") != std::string::npos);
    CHECK(frame.find("1. small") != std::string::npos);
    CHECK(frame.find("2. large") != std::string::npos);
    CHECK(frame.find("the assistant asks") != std::string::npos);
    CHECK(frame.find("2 questions") != std::string::npos);
}

TEST_CASE("ask modal: a subagent's ask names the agent", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Scope", "which scope?", {"a"})}, "review", theme, minimal_test::CoreWidgetKit(),
                              replies.sink());
    CHECK(RenderFrame(modal).find("review asks") != std::string::npos);
}

TEST_CASE("ask modal: long option descriptions and headers wrap instead of clipping",
          "[ask_view]") {
    const auto theme = TestTheme();
    auto single = Q("A header long enough to need wrapping ENDMARKZERO", "which approach?",
                    {"Trace lease path", "Quick check"});
    single.options[0].description =
        "Trace unrequested lease and durable-handle state machine transitions across the "
        "reconnect path to find where the grant silently outlives its lease token ENDMARKONE";
    auto multi = Q("Zones", "which zones?", {"zone A", "zone B"}, /*multiple=*/true);
    multi.options[0].description =
        "Identify the zone backing the buffer and size the free-list scan so the checker "
        "does not walk pages the allocator has already reclaimed ENDMARKTWO";
    Replies replies;
    auto modal = MakeAskModal({single, multi}, "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    const auto frame = RenderFrame(modal, 60, 50);
    CHECK(frame.find("ENDMARKZERO") != std::string::npos);
    CHECK(frame.find("ENDMARKONE") != std::string::npos);
    CHECK(frame.find("ENDMARKTWO") != std::string::npos);
}

// --- picking -----------------------------------------------------------------

TEST_CASE("ask modal: Enter picks the highlighted row -- row 0 included", "[ask_view]") {
    // Regression: FTXUI's render-time clamp already made row 0 "selected",
    // so its on_change never fired and a deliberate pick of row 0 read as
    // unanswered. And Enter used to go to review without picking anything.
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"})}, "", theme, minimal_test::CoreWidgetKit(),
                              replies.sink());
    RenderFrame(modal);   // the clamp
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    // A lone single-pick question submits at once -- no review step.
    REQUIRE(replies.sent.size() == 1);
    CHECK(replies.sent[0].outcome == v1::PROMPT_OUTCOME_ANSWERED);
    CHECK(replies.slot(0).at("selected") == nlohmann::json::array({"one"}));
}

TEST_CASE("ask modal: space on the clamped row 0 records a pick", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"}),
                               Q("Scope", "which scope?", {"a", "b"}, /*multiple=*/true)},
                              "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    RenderFrame(modal);
    REQUIRE(SendEvent(modal, ftx::Event::Character(" ")));
    REQUIRE(SendEvent(modal, ftx::Event::Tab));      // on to question 2, untouched
    REQUIRE(SendEvent(modal, ftx::Event::Return));   // question 2 -> review
    CHECK(RenderFrame(modal).find("-> one") != std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::Return));   // submit
    CHECK(replies.slot(0).at("selected") == nlohmann::json::array({"one"}));
}

TEST_CASE("ask modal: digit keys pick a single row and toggle checkboxes", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two", "three"}),
                               Q("Zones", "which zones?", {"A", "B", "C"}, /*multiple=*/true)},
                              "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // pick "three", on to question 2
    REQUIRE(SendEvent(modal, ftx::Event::Character("1")));   // toggle A
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // toggle C
    REQUIRE(SendEvent(modal, ftx::Event::Return));           // -> review
    REQUIRE(replies.sent.empty());
    REQUIRE(SendEvent(modal, ftx::Event::Return));           // submit
    REQUIRE(replies.sent.size() == 1);
    CHECK(replies.slot(0).at("selected") == nlohmann::json::array({"three"}));
    CHECK(replies.slot(1).at("selected") == nlohmann::json::array({"A", "C"}));
}

TEST_CASE("ask modal: a question skipped with Tab stays unanswered, not row 0",
          "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"}),
                               Q("Scope", "which scope?", {"small", "large"})},
                              "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    RenderFrame(modal);
    REQUIRE(SendEvent(modal, ftx::Event::Tab));      // skip question 1
    REQUIRE(SendEvent(modal, ftx::Event::Return));   // pick "small" -> review
    const auto review = RenderFrame(modal);
    CHECK(review.find("(not answered)") != std::string::npos);
    CHECK(review.find("-> small") != std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    CHECK(replies.slot(0).at("selected").empty());
    CHECK(replies.slot(1).at("selected") == nlohmann::json::array({"small"}));
}

// --- free text -----------------------------------------------------------------

TEST_CASE("ask modal: the free-text line exists only while \"Type your own answer\" is "
          "picked, and Enter there submits it",
          "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"})}, "", theme, minimal_test::CoreWidgetKit(),
                              replies.sink());
    // The placeholder ("your answer", no "own") is distinct from the row label.
    CHECK(RenderFrame(modal).find("your answer") == std::string::npos);

    REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));
    REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));   // highlight "Type your own answer"
    REQUIRE(SendEvent(modal, ftx::Event::Return));      // pick it: focus moves into the line
    CHECK(RenderFrame(modal).find("your answer") != std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::Return));      // empty: not an answer, stays
    CHECK(replies.sent.empty());
    REQUIRE(SendEvent(modal, ftx::Event::Character("h")));
    REQUIRE(SendEvent(modal, ftx::Event::Character("i")));
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(replies.sent.size() == 1);
    CHECK(replies.slot(0).at("other") == "hi");
    CHECK(replies.slot(0).at("selected").empty());
}

TEST_CASE("ask modal: Escape in the free-text line returns to the list; a real pick then "
          "clears the typed answer",
          "[ask_view]") {
    // Regression: Escape anywhere used to dismiss the whole ask, so leaving
    // the text line threw every answer away.
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"})}, "", theme, minimal_test::CoreWidgetKit(),
                              replies.sink());
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // "Type your own answer"
    REQUIRE(SendEvent(modal, ftx::Event::Character("h")));
    REQUIRE(SendEvent(modal, ftx::Event::Escape));           // back to the list
    CHECK(replies.sent.empty());
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));          // highlight "two"
    REQUIRE(SendEvent(modal, ftx::Event::Return));           // pick it -> submit
    REQUIRE(replies.sent.size() == 1);
    CHECK(replies.slot(0).at("selected") == nlohmann::json::array({"two"}));
    CHECK(replies.slot(0).at("other") == "");
}

TEST_CASE("ask modal: checking \"other\" clears real checkbox picks", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Zones", "which zones?", {"zone A", "zone B"}, true)}, "",
                              theme, minimal_test::CoreWidgetKit(), replies.sink());
    REQUIRE(SendEvent(modal, ftx::Event::Character(" ")));   // zone A
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // "other": clears zone A
    REQUIRE(SendEvent(modal, ftx::Event::Return));           // empty text: into the line
    REQUIRE(SendEvent(modal, ftx::Event::Character("x")));
    REQUIRE(SendEvent(modal, ftx::Event::Return));           // -> review (multi-select)
    const auto review = RenderFrame(modal);
    CHECK(review.find("your answer: x") != std::string::npos);
    CHECK(review.find("zone A") == std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    CHECK(replies.slot(0).at("other") == "x");
    CHECK(replies.slot(0).at("selected").empty());
}

TEST_CASE("ask modal: allow_other=false offers no free-text row", "[ask_view]") {
    const auto theme = TestTheme();
    auto q = Q("Approach", "which approach?", {"one", "two"});
    q.allow_other = false;
    Replies replies;
    auto modal = MakeAskModal({q}, "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    CHECK(RenderFrame(modal).find("Type your own answer") == std::string::npos);
}

// --- review and dismissal ----------------------------------------------------

TEST_CASE("ask modal: review freezes the picks; Escape peels one level", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("Approach", "which approach?", {"one", "two"}),
                               Q("Scope", "how much scope?", {"small", "large"})},
                              "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    REQUIRE(SendEvent(modal, ftx::Event::Character("2")));   // "two", on to question 2
    REQUIRE(SendEvent(modal, ftx::Event::Tab));              // skip question 2 -> review
    const auto review = RenderFrame(modal);
    CHECK(review.find("-> two") != std::string::npos);
    CHECK(review.find("(not answered)") != std::string::npos);
    CHECK(review.find("enter: submit") != std::string::npos);
    CHECK(review.find("small") == std::string::npos);   // widgets frozen away

    CHECK(SendEvent(modal, ftx::Event::ArrowDown));         // frozen: consumed
    CHECK(SendEvent(modal, ftx::Event::Character("1")));    // frozen: consumed
    CHECK_FALSE(SendEvent(modal, ftx::Event::CtrlC));       // the host's turn-cancel
    REQUIRE(SendEvent(modal, ftx::Event::Escape));          // -> editing
    CHECK(replies.sent.empty());
    CHECK(RenderFrame(modal).find("enter: pick") != std::string::npos);
}

TEST_CASE("ask modal: Escape dismisses at once when nothing is answered, and needs a "
          "second press once something is",
          "[ask_view]") {
    const auto theme = TestTheme();
    SECTION("nothing answered") {
        Replies replies;
        auto modal = MakeAskModal({Q("A", "q?", {"one"}), Q("B", "q2?", {"x"})}, "", theme, minimal_test::CoreWidgetKit(),
                                  replies.sink());
        REQUIRE(SendEvent(modal, ftx::Event::Escape));
        REQUIRE(replies.sent.size() == 1);
        CHECK(replies.sent[0].outcome == v1::PROMPT_OUTCOME_DISMISSED);
    }
    SECTION("an answer entered") {
        Replies replies;
        auto modal = MakeAskModal({Q("A", "q?", {"one"}), Q("B", "q2?", {"x"})}, "", theme, minimal_test::CoreWidgetKit(),
                                  replies.sink());
        REQUIRE(SendEvent(modal, ftx::Event::Character("1")));
        REQUIRE(SendEvent(modal, ftx::Event::Escape));   // armed
        CHECK(replies.sent.empty());
        CHECK(RenderFrame(modal).find("esc again") != std::string::npos);
        REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));   // any other key disarms
        REQUIRE(SendEvent(modal, ftx::Event::Escape));      // armed again
        CHECK(replies.sent.empty());
        REQUIRE(SendEvent(modal, ftx::Event::Escape));      // dismissed
        REQUIRE(replies.sent.size() == 1);
        CHECK(replies.sent[0].outcome == v1::PROMPT_OUTCOME_DISMISSED);
    }
}

TEST_CASE("ask modal: only one reply is ever sent", "[ask_view]") {
    const auto theme = TestTheme();
    Replies replies;
    auto modal = MakeAskModal({Q("A", "q?", {"one", "two"})}, "", theme, minimal_test::CoreWidgetKit(), replies.sink());
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    SendEvent(modal, ftx::Event::Return);
    SendEvent(modal, ftx::Event::Escape);
    CHECK(replies.sent.size() == 1);
}

// --- the renderer's payload contract -----------------------------------------

TEST_CASE("ask prompt renderer: a payload that is not an ask request throws", "[ask_view]") {
    const auto theme = TestTheme();
    minimal::ask_view::AskPromptRenderer renderer(&minimal_test::CoreWidgetKit());
    CHECK(renderer.kind() == "ask");
    const auto make = [&](const nlohmann::json& payload) {
        return renderer.Make(plugins::PromptView{.kind = "ask", .payload = payload, .asker = ""},
                             theme, [](plugins::PromptReply) {});
    };
    CHECK_THROWS(make(nullptr));
    CHECK_THROWS(make(nlohmann::json{{"questions", nlohmann::json::array()}}));
    CHECK_THROWS(make(nlohmann::json::parse(R"({"questions":[{"question":"q","options":[]}]})")));
    CHECK_THROWS(make(nlohmann::json::parse(
        R"({"questions":[{"question":"q","multiple":"yes","options":[{"label":"a"}]}]})")));
    CHECK(make(nlohmann::json::parse(
              R"({"questions":[{"question":"q","header":"h","multiple":false,
                  "allow_other":true,"options":[{"label":"a","description":""}]}]})")) !=
          nullptr);
}

TEST_CASE("RadioRowSelected: only a touched question styles its current row", "[ask_view]") {
    using minimal::ask_view::RadioRowSelected;
    CHECK_FALSE(RadioRowSelected(/*touched=*/false, /*active=*/true, /*focused=*/true));
    CHECK(RadioRowSelected(true, true, false));
    CHECK(RadioRowSelected(true, false, true));
    CHECK_FALSE(RadioRowSelected(true, false, false));
}

// --- the transcript row --------------------------------------------------------

namespace {

nlohmann::json AskDisplay(const std::string& outcome, const std::string& other = {}) {
    return nlohmann::json{
        {"kind", "ask"},
        {"outcome", outcome},
        {"questions",
         nlohmann::json::array({nlohmann::json{{"header", "Approach"},
                                               {"question", "which approach?"},
                                               {"selected", other.empty()
                                                                ? nlohmann::json::array({"one"})
                                                                : nlohmann::json::array()},
                                               {"other", other}}})}};
}

}  // namespace

TEST_CASE("ask display: the row records each question and its answer", "[ask_view]") {
    const auto theme = TestTheme();
    minimal::ask_view::AskDisplayRenderer renderer;
    CHECK(renderer.collapsed_by_default() == false);

    const nlohmann::json input = nlohmann::json::object();
    const auto display = AskDisplay("answered");
    const plugins::ToolResultView view{.tool_name = "ask",
                                       .tool_input = input,
                                       .display = display,
                                       .result_text = "1. Approach: the user selected: one"};
    const auto expanded = RenderElement(renderer.Render(view, theme, {}));
    CHECK(expanded.find("ask") != std::string::npos);   // the (aliased) tool name
    CHECK(expanded.find("Approach: which approach?") != std::string::npos);
    CHECK(expanded.find("-> one") != std::string::npos);

    plugins::RowRenderContext collapsed;
    collapsed.collapsed = true;
    const auto line = RenderElement(renderer.Render(view, theme, collapsed));
    CHECK(line.find("which approach?  -> one") != std::string::npos);

    const auto dismissed_display = AskDisplay("timed_out");
    const plugins::ToolResultView dismissed{.tool_name = "ask_user",
                                            .tool_input = input,
                                            .display = dismissed_display,
                                            .result_text = "..."};
    CHECK(RenderElement(renderer.Render(dismissed, theme, {})).find("unattended timeout") !=
          std::string::npos);
}

TEST_CASE("chat view: an ask row starts expanded by its display kind, whatever the tool "
          "is called",
          "[ask_view]") {
    // Regression: the expanded default was keyed by the literal tool name
    // "ask_user", so an operator alias silently collapsed the row.
    const auto theme = TestTheme();
    plugins::MarkdownEngineRegistry engine_registry;
    minimal::client::markdown_md4c::RegisterEngine(engine_registry, nullptr, nullptr);
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::DisplayRendererRegistry display_renderers;
    minimal::client::chat_renderers::RegisterMessageRenderers(messages, *engine_registry.get());
    display_renderers.Register(std::make_unique<minimal::ask_view::AskDisplayRenderer>());

    v1::StoredMessage call;
    call.set_seq(1);
    call.mutable_message()->set_role(v1::ROLE_ASSISTANT);
    auto* use = call.mutable_message()->add_content()->mutable_tool_use();
    use->set_id("call-ask-1");
    use->set_name("ask");   // aliased
    v1::StoredMessage result;
    result.set_seq(2);
    result.mutable_message()->set_role(v1::ROLE_USER);
    auto* tool_result = result.mutable_message()->add_content()->mutable_tool_result();
    tool_result->set_tool_use_id("call-ask-1");
    tool_result->add_content()->mutable_text()->set_text("1. Approach: the user selected: one");
    tool_result->set_display_json(AskDisplay("answered").dump());

    minimal_test::ChatViewHarness view(theme, messages, tool_renderers, display_renderers);
    SessionState::Snapshot snap;
    snap.session_id = "session-ask";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::vector<v1::StoredMessage>{result, call});
    const auto frame = RenderElement(view.Render(snap), 100, 30);
    CHECK(frame.find("Approach: which approach?") != std::string::npos);
    CHECK(frame.find("-> one") != std::string::npos);
}
