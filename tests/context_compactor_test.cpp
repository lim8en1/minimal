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

agent::CompactionResult RunOk(FakeLlmProvider& provider, std::int64_t last_reported) {
    const auto result = agent::compact_messages(MixedHistory(), provider, "model-x",
                                                "THE PROMPT", last_reported, {});
    REQUIRE(result.has_value());
    return *result;
}

}  // namespace

TEST_CASE("compact_messages sends the prompt as system and passes messages through verbatim",
          "[compactor]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("SUMMARY TEXT"));

    const auto result = RunOk(provider, 1234);

    CHECK(result.original_token_count == 1234);
    // The summary is a user message: it stands in for the compacted prefix of the
    // conversation, and that is the shape both wire formats accept at the head.
    REQUIRE(result.summary_message.role == llm::Role::User);
    REQUIRE(result.summary_message.content.size() == 1);
    const auto& text = std::get<llm::TextBlock>(result.summary_message.content.front()).text;
    CHECK(text.ends_with("SUMMARY TEXT"));

    REQUIRE(provider.requests().size() == 1);
    const auto& request = provider.requests().front();
    REQUIRE(request.system.has_value());
    CHECK(*request.system == "THE PROMPT");
    // Raw pass-through: tool blocks included, no flattening. (No synthesized
    // operator== on Message under the pinned toolchain, so compare structurally.)
    const auto expected = MixedHistory();
    REQUIRE(request.messages.size() == expected.size());
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
    CHECK(request.model == "model-x");
    CHECK(request.tools.empty());
    CHECK(request.stream);
}

TEST_CASE("original_token_count falls back to a bytes/4 estimate when no usage was reported",
          "[compactor]") {
    FakeLlmProvider provider;
    provider.PushResponse(TextResponse("ok"));

    const std::vector<llm::Message> messages = {UserText(std::string(4000, 'x'))};
    const auto result = agent::compact_messages(messages, provider, "model-x", "THE PROMPT", 0, {});

    REQUIRE(result.has_value());
    CHECK(result->original_token_count == 1000);  // 4000 bytes / 4
}

TEST_CASE("a retryable provider error degrades to no compaction", "[compactor]") {
    FakeLlmProvider provider;
    provider.PushError(llm::LlmError(llm::LlmErrorCategory::Transport, "connection reset"));

    const auto result = agent::compact_messages(MixedHistory(), provider, "model-x", "THE PROMPT",
                                                10, {});

    CHECK_FALSE(result.has_value());
}

TEST_CASE("a non-retryable provider error degrades to no compaction", "[compactor]") {
    FakeLlmProvider provider;
    provider.PushError(llm::LlmError(llm::LlmErrorCategory::RequestError, "context too long"));

    const auto result = agent::compact_messages(MixedHistory(), provider, "model-x", "THE PROMPT",
                                                10, {});

    CHECK_FALSE(result.has_value());
}

TEST_CASE("an empty prompt skips the provider entirely", "[compactor]") {
    FakeLlmProvider provider;

    const auto result = agent::compact_messages(MixedHistory(), provider, "model-x", "   \n\t",
                                                10, {});

    CHECK_FALSE(result.has_value());
    CHECK(provider.call_count() == 0);
}

TEST_CASE("a response without text blocks is unusable", "[compactor]") {
    FakeLlmProvider provider;
    llm::ChatResponse weird;
    weird.content.push_back(llm::ToolUseBlock{"t1", "read", nlohmann::json::object()});
    weird.stop_reason = llm::StopReason::EndTurn;
    provider.PushResponse(std::move(weird));

    const auto result = agent::compact_messages(MixedHistory(), provider, "model-x", "THE PROMPT",
                                                10, {});

    CHECK_FALSE(result.has_value());
}

TEST_CASE("cancellation aborts the summarization call and yields no compaction", "[compactor]") {
    FakeLlmProvider provider;
    // The step carries an event so the sink is actually consulted; a real provider
    // would be mid-transfer here.
    provider.PushResponse(TextResponse("never assembled"),
                          {llm::TextDeltaEvent{0, "partial"}});

    const auto result = agent::compact_messages(
        MixedHistory(), provider, "model-x", "THE PROMPT", 10, [] { return true; });

    CHECK_FALSE(result.has_value());
}
