// The "structured" compaction strategy: the forced record_summary tool call,
// the schema-driven parse, and the plain-text render. The shared half (cut
// selection, request shape, refusal chain) is compact_messages' and is only
// re-asserted here where the two could drift.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

#include "fakes.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/structured_compaction.hpp"

using namespace minimal;
using namespace minimal::agent;
using namespace minimal::testing;

namespace {

llm::Message LocalUserText(const std::string& text) {
    return llm::Message{llm::Role::User, {llm::TextBlock{text}}};
}

// The tool-call response a conforming summarizer returns: one record_summary
// call carrying the schema fields as its input.
llm::ChatResponse SummaryResponse(nlohmann::json input) {
    llm::ChatResponse response;
    response.content.push_back(
        llm::ToolUseBlock{"sum1", "record_summary", std::move(input)});
    response.stop_reason = llm::StopReason::ToolUse;
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    return response;
}

agent::CompactionRequest Params(std::int64_t last_reported = 0) {
    agent::CompactionRequest params;
    params.model_name = "model-x";
    params.compression_prompt = "THE PROMPT";
    params.structured_prompt = "STRUCTURED PROMPT";
    params.last_reported_input_tokens = last_reported;
    return params;
}

// One tool-call cycle plus an assistant turn: enough history that the
// pass-through assertions are about tool blocks actually present.
std::vector<llm::Message> MixedHistory() {
    std::vector<llm::Message> messages;
    messages.push_back(UserText("read the file for me"));
    messages.push_back(llm::Message{llm::Role::Assistant,
                                    {llm::ToolUseBlock{"tu1", "read",
                                                       nlohmann::json{{"path", "a.txt"}}}}});
    messages.push_back(llm::Message{
        llm::Role::User, {llm::ToolResultBlock{"tu1", {llm::TextBlock{"file contents"}}, false}}});
    messages.push_back(llm::Message{llm::Role::Assistant, {llm::TextBlock{"here you go"}}});
    return messages;
}

const llm::ChatRequest& SingleRequest(const FakeLlmProvider& provider) {
    REQUIRE(provider.requests().size() == 1);
    return provider.requests().front();
}

}  // namespace

TEST_CASE("RenderStructuredSummary: section order and byte shape", "[structured][render]") {
    StructuredSummary summary;
    summary.current_task = "refactoring the compactor";
    summary.done.push_back("wrote src/compactor.cpp");
    summary.next_steps.push_back("run the tests");
    summary.topics.push_back("compaction");
    summary.important.push_back(ImportantItem{"src/compactor.cpp", "the code under change",
                                              "keep the refusal chain intact"});
    summary.lessons.push_back("the loop is worker-thread-only");
    summary.future_tasks.push_back("port to the client");

    const std::string rendered = RenderStructuredSummary(summary);

    // The framing line, then one "## Section" per non-empty field in declared
    // order -- the declared order IS the render order.
    CHECK(rendered.starts_with(kSummaryFraming));
    const std::string body = rendered.substr(kSummaryFraming.size());
    CHECK(body.starts_with("## Current task\nrefactoring the compactor\n"));
    CHECK(body.find("## Done\n- wrote src/compactor.cpp\n") != std::string::npos);
    CHECK(body.find("## Next steps\n- run the tests\n") != std::string::npos);
    CHECK(body.find("## Topics\n- compaction\n") != std::string::npos);
    CHECK(body.find("## Important\n- src/compactor.cpp -- the code under change (how: keep "
                    "the refusal chain intact)\n") != std::string::npos);
    CHECK(body.find("## Lessons\n- the loop is worker-thread-only\n") != std::string::npos);
    CHECK(body.find("## Future tasks\n- port to the client\n") != std::string::npos);

    // Sections appear exactly once, in declared order.
    const std::size_t task = body.find("## Current task");
    const std::size_t done = body.find("## Done");
    const std::size_t next = body.find("## Next steps");
    const std::size_t topics = body.find("## Topics");
    const std::size_t important = body.find("## Important");
    const std::size_t lessons = body.find("## Lessons");
    const std::size_t future = body.find("## Future tasks");
    CHECK(task < done);
    CHECK(done < next);
    CHECK(next < topics);
    CHECK(topics < important);
    CHECK(important < lessons);
    CHECK(lessons < future);
}

TEST_CASE("RenderStructuredSummary: empty sections are omitted, empty how is omitted",
          "[structured][render]") {
    StructuredSummary summary;
    summary.current_task = "the task";
    summary.important.push_back(ImportantItem{"a.txt", "because", ""});

    const std::string rendered = RenderStructuredSummary(summary);

    CHECK(rendered.find("## Done") == std::string::npos);
    CHECK(rendered.find("## Lessons") == std::string::npos);
    CHECK(rendered.find("- a.txt -- because\n") != std::string::npos);
    CHECK(rendered.find("(how:") == std::string::npos);
}

TEST_CASE("RenderStructuredSummary: a missing current_task renders the visible marker",
          "[structured][render]") {
    // The degrade path: six good fields survive a missing keystone, but the
    // continuing model must SEE that it is missing, not find silence.
    StructuredSummary summary;
    summary.done.push_back("something finished");

    const std::string rendered = RenderStructuredSummary(summary);
    CHECK(rendered.find("(current task: not reported)") != std::string::npos);
    CHECK(rendered.find("## Done") != std::string::npos);
}

TEST_CASE("compact_messages_structured sends the session prefix plus an instruction, with "
          "record_summary offered but not forced",
          "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(
        SummaryResponse(nlohmann::json{{"current_task", "the task"}}));

    auto params = Params(1234);
    params.session_id = "s1";
    params.system = "SESSION SYSTEM";
    params.tools = {llm::ToolDefinition{"read", "reads", nlohmann::json{{"type", "object"}}}};
    const auto result = compact_messages_structured(MixedHistory(), provider, params);
    REQUIRE(result.has_value());
    CHECK(result->context_tokens_before == 1234);

    const auto& request = SingleRequest(provider);
    REQUIRE(request.system.has_value());
    CHECK(*request.system == "SESSION SYSTEM");
    // The session's tools plus record_summary, sorted -- a loop configured with
    // offer_summary_tool already carries it and the array is left untouched.
    REQUIRE(request.tools.size() == 2);
    CHECK(request.tools[0].name == "read");
    CHECK(request.tools[1].name == "record_summary");
    CHECK(request.tools[1].strict);
    CHECK(request.tools[1].input_schema["properties"].contains("current_task"));
    CHECK(request.tools[1].input_schema["required"].size() == 7);
    // Offered, never forced: a forced tool_choice invalidates the messages
    // cache and is rejected outright by newer Claude models.
    CHECK(request.tool_choice.kind == llm::ToolChoiceKind::Auto);
    // Same sampling/routing passthrough contract as compact_messages.
    CHECK(request.model == "model-x");
    CHECK(request.session_id == "s1");
    CHECK(request.stream);

    // Raw pass-through of the WHOLE history, then one instruction message
    // naming the tool.
    const auto expected = MixedHistory();
    REQUIRE(request.messages.size() == expected.size() + 1);
    const auto* use = std::get_if<llm::ToolUseBlock>(&request.messages[1].content.front());
    REQUIRE(use != nullptr);
    CHECK(use->id == "tu1");
    // prompts/compression-structured.md, verbatim.
    const auto& instruction = std::get<llm::TextBlock>(request.messages.back().content.front()).text;
    CHECK(instruction == "STRUCTURED PROMPT");
}

TEST_CASE("compact_messages_structured without a structured prompt runs as free text",
          "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("plain summary"));
    auto params = Params(1);
    params.structured_prompt = "";
    const auto result = compact_messages_structured(MixedHistory(), provider, params);
    REQUIRE(result.has_value());
    const auto& request = SingleRequest(provider);
    CHECK(request.tools.empty());
    CHECK(std::get<llm::TextBlock>(request.messages.back().content.front()).text == "THE PROMPT");
}

TEST_CASE("compact_messages_structured leaves an already-offered record_summary alone",
          "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{{"current_task", "t"}}));
    auto params = Params(1);
    params.tools = {llm::ToolDefinition{"a", "", nlohmann::json::object()},
                    MakeRecordSummaryToolDefinition()};
    REQUIRE(compact_messages_structured(MixedHistory(), provider, params).has_value());
    const auto& request = SingleRequest(provider);
    REQUIRE(request.tools.size() == 2);
    CHECK(request.tools[1].name == "record_summary");
}

TEST_CASE("compact_messages_structured runs a custom prompt as free text", "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("custom-shaped summary"));
    auto params = Params(1);
    params.custom_prompt = true;
    const auto result = compact_messages_structured(MixedHistory(), provider, params);
    REQUIRE(result.has_value());
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    CHECK(text.ends_with("custom-shaped summary"));
    const auto& request = SingleRequest(provider);
    CHECK(request.tools.empty());
    const auto& instruction = std::get<llm::TextBlock>(request.messages.back().content.front()).text;
    CHECK(instruction.starts_with("THE PROMPT"));
}

TEST_CASE("compact_messages_structured parses the tool call into the summary head",
          "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{
        {"current_task", "shipping structured compaction"},
        {"done", nlohmann::json::array({"wrote the schema", "parsed the call"})},
        {"next_steps", nlohmann::json::array({"run tests"})},
        {"topics", nlohmann::json::array({"compaction"})},
        {"important",
         nlohmann::json::array({nlohmann::json{{"name", "context_compactor.hpp"},
                                               {"why", "owns the shared half"},
                                               {"how", "read before touching the compactor"}}})},
        {"lessons", nlohmann::json::array({"refusals must degrade"})},
        {"future_tasks", nlohmann::json::array({"kv-cache reuse"})}}));

    const auto result = compact_messages_structured(MixedHistory(), provider, Params(50));
    REQUIRE(result.has_value());
    REQUIRE(result->summary_message.role == llm::Role::User);
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;

    CHECK(text.find("## Current task\nshipping structured compaction\n") != std::string::npos);
    CHECK(text.find("- wrote the schema") != std::string::npos);
    CHECK(text.find("- run tests") != std::string::npos);
    CHECK(text.find("## Topics\n- compaction\n") != std::string::npos);
    CHECK(text.find("- context_compactor.hpp -- owns the shared half (how: read before "
                    "touching the compactor)") != std::string::npos);
    CHECK(text.find("- refusals must degrade") != std::string::npos);
    CHECK(text.find("- kv-cache reuse") != std::string::npos);
}

TEST_CASE("compact_messages_structured: missing current_task degrades with a marker",
          "[structured][invariant]") {
    // A refusal here re-fires next turn while occupancy climbs toward a hard
    // 400 -- and no portable enforcement story exists for the schema's
    // `required` across backends. Six good fields must not die for the one.
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{
        {"done", nlohmann::json::array({"the work"})},
    }));

    const auto result = compact_messages_structured(MixedHistory(), provider, Params(10));
    REQUIRE(result.has_value());
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    CHECK(text.find("(current task: not reported)") != std::string::npos);
    CHECK(text.find("- the work") != std::string::npos);
}

TEST_CASE("compact_messages_structured filters malformed entries with the rest intact",
          "[structured][invariant]") {
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{
        {"current_task", "the task"},
        {"done", nlohmann::json::array({"good item", 42, "another good one"})},
        {"topics", "not an array"},
        {"important",
         nlohmann::json::array({nlohmann::json{{"why", "no name"}},
                                nlohmann::json{{"name", "keeper.txt"}, {"why", "named"}}})},
    }));

    const auto result = compact_messages_structured(MixedHistory(), provider, Params(10));
    REQUIRE(result.has_value());
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    // Non-string array items are skipped; string items around them survive.
    CHECK(text.find("- good item") != std::string::npos);
    CHECK(text.find("- another good one") != std::string::npos);
    // A non-array field is ignored whole.
    CHECK(text.find("## Topics") == std::string::npos);
    // A nameless important entry is skipped; the named one survives.
    CHECK(text.find("keeper.txt -- named") != std::string::npos);
    CHECK(text.find("(no name)") == std::string::npos);
}

TEST_CASE("compact_messages_structured: a tool call with no usable fields is refused",
          "[structured][invariant]") {
    // A header skeleton must not replace half the conversation.
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json::object()));

    CHECK_FALSE(compact_messages_structured(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("compact_messages_structured: unparseable tool-call arguments are refused",
          "[structured][invariant]") {
    FakeLlmProvider provider;
    llm::ChatResponse response;
    response.content.push_back(
        llm::ToolUseBlock{"sum1", "record_summary", nlohmann::json{}});  // not an object
    response.stop_reason = llm::StopReason::ToolUse;
    provider.PushResponse(std::move(response));

    CHECK_FALSE(compact_messages_structured(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("compact_messages_structured refuses a truncated summary before parsing",
          "[structured][invariant]") {
    // Truncated args are precisely the unparseable-JSON case; refusing at the
    // stop-reason check reports the real cause instead of a misleading parse
    // failure -- the same rule compact_messages applies to free text.
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("The user asked me to refac",
                                       llm::StopReason::MaxTokens));

    CHECK_FALSE(compact_messages_structured(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("compact_messages_structured: a model that did not call the tool degrades to the "
          "free-text head",
          "[structured][invariant]") {
    // A non-conforming backend must degrade to yesterday's behavior, not
    // turn every compaction into a failure spiral.
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("the free-text summary"));

    auto params = Params(99);
    const auto result = compact_messages_structured(MixedHistory(), provider, params);
    REQUIRE(result.has_value());
    CHECK(result->context_tokens_before == 99);
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    CHECK(text.starts_with(kSummaryFraming));
    CHECK(text.ends_with("the free-text summary"));
}

TEST_CASE("compact_messages_structured: no tool call and no text is refused",
          "[structured]") {
    FakeLlmProvider provider;
    llm::ChatResponse weird;
    weird.content.push_back(llm::ToolUseBlock{"t1", "read", nlohmann::json::object()});
    weird.stop_reason = llm::StopReason::EndTurn;
    provider.PushResponse(std::move(weird));

    CHECK_FALSE(compact_messages_structured(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("compact_messages_structured shares compact_messages' refusal chain", "[structured]") {
    // Blank custom prompt: skip the provider entirely. (The default prompt is
    // not used by the structured path, so only a custom one can be blank.)
    FakeLlmProvider blank;
    auto blank_params = Params(10);
    blank_params.compression_prompt = "  \n\t";
    blank_params.custom_prompt = true;
    CHECK_FALSE(compact_messages_structured(MixedHistory(), blank, blank_params).has_value());
    CHECK(blank.call_count() == 0);

    // Provider error (retryable or not): degrade to no compaction.
    FakeLlmProvider transport;
    transport.PushError(llm::LlmError(llm::LlmErrorCategory::Transport, "connection reset"));
    CHECK_FALSE(compact_messages_structured(MixedHistory(), transport, Params(10)).has_value());

    FakeLlmProvider request_error;
    request_error.PushError(llm::LlmError(llm::LlmErrorCategory::RequestError, "context too long"));
    CHECK_FALSE(
        compact_messages_structured(MixedHistory(), request_error, Params(10)).has_value());

    // Cancellation aborts the call and yields no compaction.
    FakeLlmProvider cancelled;
    cancelled.PushResponse(TextResponse("never assembled"), {llm::TextDeltaEvent{0, "partial"}});
    auto cancel_params = Params(10);
    cancel_params.is_cancelled = [] { return true; };
    CHECK_FALSE(compact_messages_structured(MixedHistory(), cancelled, cancel_params).has_value());
}

TEST_CASE("compact_messages_structured carries the preset's sampling settings", "[structured]") {
    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{{"current_task", "t"}}));

    auto params = Params(10);
    params.parameters = nlohmann::json{{"temperature", 0.1}};
    params.max_tokens = 2048;
    REQUIRE(compact_messages_structured(MixedHistory(), provider, params).has_value());

    const auto& request = SingleRequest(provider);
    CHECK(request.parameters == nlohmann::json{{"temperature", 0.1}});
    REQUIRE(request.max_tokens.has_value());
    CHECK(*request.max_tokens == 2048);
}

TEST_CASE("both branches of context_tokens_before measure the same span under the structured "
          "compactor",
          "[structured][invariant]") {
    const std::vector<llm::Message> full = {LocalUserText(std::string(4000, 'x')),
                                            LocalUserText(std::string(4000, 'y'))};
    const std::vector<llm::Message> cut(full.begin(), full.begin() + 1);

    FakeLlmProvider estimated_provider;
    estimated_provider.PushResponse(SummaryResponse(nlohmann::json{{"current_task", "t"}}));
    auto estimated = Params(0);
    estimated.full_history = &full;
    const auto without_usage =
        compact_messages_structured(cut, estimated_provider, estimated);

    FakeLlmProvider reporting_provider;
    reporting_provider.PushResponse(SummaryResponse(nlohmann::json{{"current_task", "t"}}));
    auto ground_truth = Params(2000);
    ground_truth.full_history = &full;
    const auto with_usage = compact_messages_structured(cut, reporting_provider, ground_truth);

    REQUIRE(without_usage.has_value());
    REQUIRE(with_usage.has_value());
    CHECK(without_usage->context_tokens_before == 2000);
    CHECK(with_usage->context_tokens_before == 2000);
}

TEST_CASE("MakeStructuredCompactionStrategy: same cut selection as summarize, structured "
          "summarizer",
          "[structured]") {
    const auto structured = MakeStructuredCompactionStrategy();
    const auto summarize = MakeSummarizeCompactionStrategy();

    REQUIRE(structured.cut_selector != nullptr);
    REQUIRE(structured.summarize != nullptr);

    // Where to cut is orthogonal to how to summarize: the strategies must
    // agree on the cut, and differ in (only) the summarizer.
    std::vector<llm::Message> history;
    for (int i = 0; i < 10; ++i) history.push_back(LocalUserText("msg"));
    CHECK(structured.cut_selector(history, 0) == summarize.cut_selector(history, 0));
    CHECK(structured.cut_selector(history, 0) == 5);
}

TEST_CASE("the structured strategy end-to-end through its registry entry", "[structured]") {
    CompactionStrategyRegistry registry;
    CHECK(registry.Register("structured", MakeStructuredCompactionStrategy()));
    const auto* strategy = registry.Find("structured");
    REQUIRE(strategy != nullptr);

    FakeLlmProvider provider;
    provider.PushResponse(SummaryResponse(nlohmann::json{{"current_task", "the task"}}));

    const auto result = strategy->summarize(MixedHistory(), provider, Params(7));
    REQUIRE(result.has_value());
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    CHECK(text.find("## Current task\nthe task\n") != std::string::npos);
    CHECK(result->context_tokens_before == 7);
}
