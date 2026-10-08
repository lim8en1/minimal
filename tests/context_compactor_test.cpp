#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "fakes.hpp"
#include "minimal/agent/context_compactor.hpp"

using namespace minimal;
using namespace minimal::testing;

namespace {

// A full tool-call cycle: user text, assistant tool_use, user tool_result,
// assistant text. The pass-through assertion is only interesting if tool blocks
// are actually present.
std::vector<llm::Message> MixedHistory() {
    std::vector<llm::Message> messages;
    messages.push_back(UserText("read the file for me"));
    llm::Message use{llm::Role::Assistant,
                     {llm::ToolUseBlock{"tu1", "read", nlohmann::json{{"path", "a.txt"}}}}};
    messages.push_back(use);
    llm::Message result{llm::Role::User,
                        {llm::ToolResultBlock{"tu1", {llm::TextBlock{"file contents"}}, false}}};
    messages.push_back(result);
    messages.push_back(llm::Message{llm::Role::Assistant, {llm::TextBlock{"here you go"}}});
    return messages;
}

// The defaults every case shares; each test overrides only what it is about.
agent::CompactionRequest Params(std::int64_t last_reported = 0) {
    agent::CompactionRequest params;
    params.model_name = "model-x";
    params.compression_prompt = "THE PROMPT";
    params.last_reported_input_tokens = last_reported;
    return params;
}

agent::CompactionResult RunOk(FakeLlmProvider& provider, std::int64_t last_reported) {
    const auto result = agent::compact_messages(MixedHistory(), provider, Params(last_reported));
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace

TEST_CASE("compact_messages sends the session prefix verbatim plus one instruction message",
          "[compactor]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("SUMMARY TEXT"));

    auto params = Params(1234);
    params.system = "SESSION SYSTEM";
    params.tools = {llm::ToolDefinition{"read", "reads", nlohmann::json{{"type", "object"}}}};
    const auto result = agent::compact_messages(MixedHistory(), provider, params);
    REQUIRE(result.has_value());

    CHECK(result->context_tokens_before == 1234);
    // The summary is a user message: it stands in for the compacted prefix of the
    // conversation, and that is the shape both wire formats accept at the head.
    REQUIRE(result->summary_message.role == llm::Role::User);
    REQUIRE(result->summary_message.content.size() == 1);
    const auto& text = std::get<llm::TextBlock>(result->summary_message.content.front()).text;
    CHECK(text.starts_with(agent::kSummaryFraming));
    CHECK(text.ends_with("SUMMARY TEXT"));

    REQUIRE(provider.requests().size() == 1);
    const auto& request = provider.requests().front();
    // The session's own system prompt and tools, so the provider's prefix
    // cache covers the history: the compression prompt is NOT the system.
    REQUIRE(request.system.has_value());
    CHECK(*request.system == "SESSION SYSTEM");
    REQUIRE(request.tools.size() == 1);
    CHECK(request.tools.front().name == "read");
    CHECK(request.tool_choice.kind == llm::ToolChoiceKind::Auto);
    // Raw pass-through of the WHOLE history, then exactly one appended user
    // message carrying the prompt. (No synthesized operator== on Message under
    // the pinned toolchain, so compare structurally.)
    const auto expected = MixedHistory();
    REQUIRE(request.messages.size() == expected.size() + 1);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(request.messages[i].role == expected[i].role);
        CHECK(request.messages[i].content.size() == expected[i].content.size());
    }
    const auto* use = std::get_if<llm::ToolUseBlock>(&request.messages[1].content.front());
    REQUIRE(use != nullptr);
    CHECK(use->id == "tu1");
    CHECK(use->name == "read");
    CHECK(use->input == nlohmann::json{{"path", "a.txt"}});
    const auto* result_block = std::get_if<llm::ToolResultBlock>(&request.messages[2].content.front());
    REQUIRE(result_block != nullptr);
    CHECK(result_block->tool_use_id == "tu1");
    const auto& instruction = request.messages.back();
    CHECK(instruction.role == llm::Role::User);
    const auto& instruction_text = std::get<llm::TextBlock>(instruction.content.front()).text;
    // Verbatim: the text lives in prompts/compression.md, not in the harness.
    CHECK(instruction_text == "THE PROMPT");
    CHECK(request.model == "model-x");
    CHECK(request.stream);
}

TEST_CASE("compact_messages with no session prefix sends no system and no tools", "[compactor]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("SUMMARY TEXT"));
    RunOk(provider, 0);
    REQUIRE(provider.requests().size() == 1);
    CHECK_FALSE(provider.requests().front().system.has_value());
    CHECK(provider.requests().front().tools.empty());
}

TEST_CASE("compact_messages forwards the session id onto the summarizer's request",
          "[compactor][session_id]") {
    // OpenRouter sticky routing: the compaction call should group with the
    // session's own traffic, whatever entry (own model or small_model) serves
    // it -- see CompactionRequest::session_id.
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("SUMMARY TEXT"));

    auto params = Params();
    params.session_id = "s1";
    REQUIRE(agent::compact_messages(MixedHistory(), provider, params).has_value());

    REQUIRE(provider.requests().size() == 1);
    CHECK(provider.requests().front().session_id == "s1");

    // No id, no field: the default CompactionRequest serializes nothing.
    FakeLlmProvider bare;
    bare.PushResponse(TextResponse("SUMMARY TEXT"));
    REQUIRE(agent::compact_messages(MixedHistory(), bare, Params()).has_value());
    CHECK(bare.requests().front().session_id.empty());
}

TEST_CASE("context_tokens_before falls back to a bytes/4 estimate when no usage was reported",
          "[compactor]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("ok"));

    const std::vector<llm::Message> messages = {UserText(std::string(4000, 'x'))};
    auto params = Params(0);
    params.full_history = &messages;
    const auto result = agent::compact_messages(messages, provider, params);

    REQUIRE(result.has_value());
    CHECK(result->context_tokens_before == 1000);  // 4000 bytes / 4
}

TEST_CASE("both branches of context_tokens_before measure the same span", "[compactor][invariant]") {
    // They did not. The ground-truth branch reported the backend's count for the
    // WHOLE request; the fallback estimated only the range being compacted. So
    // the same session produced numbers differing by roughly the size of
    // everything compaction kept, depending on whether the backend happened to
    // report usage -- and the log printed either as "what was replaced".
    const std::vector<llm::Message> full = {UserText(std::string(4000, 'x')),
                                            UserText(std::string(4000, 'y'))};
    const std::vector<llm::Message> cut(full.begin(), full.begin() + 1);

    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("ok"));
    auto estimated = Params(0);
    estimated.full_history = &full;
    const auto without_usage = agent::compact_messages(cut, provider, estimated);

    FakeLlmProvider reporting;
    reporting.PushResponse(TextResponse("ok"));
    auto ground_truth = Params(2000);  // what the backend says the whole request cost
    ground_truth.full_history = &full;
    const auto with_usage = agent::compact_messages(cut, reporting, ground_truth);

    REQUIRE(without_usage.has_value());
    REQUIRE(with_usage.has_value());
    // 8000 bytes of history / 4 == 2000, the same span the backend counted.
    CHECK(without_usage->context_tokens_before == 2000);
    CHECK(with_usage->context_tokens_before == 2000);
}

TEST_CASE("a truncated summary is refused", "[compactor][invariant]") {
    // A summary that hit max_tokens is cut off mid-sentence, and accepting it
    // replaces half the conversation with a fragment whose only other copy is in
    // SQLite where the loop cannot reach it. The agent loop already refuses to
    // treat MaxTokens as normal completion; this is the same rule where the
    // consequence is worse.
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("The user asked me to refac",
                                       llm::StopReason::MaxTokens));

    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("the summarizer's usage is reported even when its summary is refused",
          "[compactor][billing]") {
    // The backend billed the call whether or not the summary is usable.
    FakeLlmProvider provider;
    auto truncated = TextResponse("The user asked me to refac", llm::StopReason::MaxTokens);
    truncated.usage.input_tokens = 300000;
    truncated.usage.output_tokens = 4096;
    provider.PushResponse(truncated);

    auto params = Params(10);
    std::vector<llm::Usage> reported;
    params.on_usage = [&reported](const llm::Usage& usage) { reported.push_back(usage); };
    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, params).has_value());
    REQUIRE(reported.size() == 1);
    CHECK(reported[0].input_tokens == 300000);
    CHECK(reported[0].output_tokens == 4096);
}

TEST_CASE("the summarization call carries the preset's sampling settings", "[compactor]") {
    // Without these the summarization call runs with different generation
    // settings than the conversation it summarizes -- and because the OpenAI
    // serializer treats max_tokens as structural and erases whatever
    // `parameters` set, an unset cap here is how the truncation above happens.
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("ok"));

    auto params = Params(10);
    params.parameters = nlohmann::json{{"temperature", 0.1}};
    params.max_tokens = 2048;
    REQUIRE(agent::compact_messages(MixedHistory(), provider, params).has_value());

    REQUIRE(provider.requests().size() == 1);
    const auto& request = provider.requests().front();
    CHECK(request.parameters == nlohmann::json{{"temperature", 0.1}});
    REQUIRE(request.max_tokens.has_value());
    CHECK(*request.max_tokens == 2048);
}

TEST_CASE("a retryable provider error degrades to no compaction", "[compactor]") {
    FakeLlmProvider provider;
    provider.PushError(llm::LlmError(llm::LlmErrorCategory::Transport, "connection reset"));

    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("a non-retryable provider error degrades to no compaction", "[compactor]") {
    FakeLlmProvider provider;
    provider.PushError(llm::LlmError(llm::LlmErrorCategory::RequestError, "context too long"));

    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("an empty prompt skips the provider entirely", "[compactor]") {
    FakeLlmProvider provider;

    auto params = Params(10);
    params.compression_prompt = "   \n\t";
    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, params).has_value());
    CHECK(provider.call_count() == 0);
}

TEST_CASE("a response without text blocks is unusable", "[compactor]") {
    FakeLlmProvider provider;
    llm::ChatResponse weird;
    weird.content.push_back(llm::ToolUseBlock{"t1", "read", nlohmann::json::object()});
    weird.stop_reason = llm::StopReason::EndTurn;
    provider.PushResponse(std::move(weird));

    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, Params(10)).has_value());
}

TEST_CASE("cancellation aborts the summarization call and yields no compaction", "[compactor]") {
    FakeLlmProvider provider;
    // The step carries an event so the sink is actually consulted; a real provider
    // would be mid-transfer here.
    provider.PushResponse(TextResponse("never assembled"),
                          {llm::TextDeltaEvent{0, "partial"}});

    auto params = Params(10);
    params.is_cancelled = [] { return true; };
    CHECK_FALSE(agent::compact_messages(MixedHistory(), provider, params).has_value());
}
