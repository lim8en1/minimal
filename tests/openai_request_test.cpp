#include <catch2/catch_test_macros.hpp>

#include "openai_request.hpp"

using namespace minimal;
using namespace minimal::provider_openai;

namespace {

llm::ChatRequest BaseRequest() {
    llm::ChatRequest request;
    request.model = "gpt-4o-mini";
    request.messages.push_back(llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}});
    return request;
}

}  // namespace

TEST_CASE("BuildRequestBody: structural fields always win over parameters", "[openai_request]") {
    llm::ChatRequest request = BaseRequest();
    request.parameters = {{"model", "bogus"}, {"temperature", 0.7}, {"stream", true}};
    request.stream = false;

    nlohmann::json body = BuildRequestBody(request);

    CHECK(body["model"] == "gpt-4o-mini");
    CHECK(body["temperature"] == 0.7);  // untouched pass-through knob survives
    CHECK(body["stream"] == false);     // ChatRequest's own member wins
}

TEST_CASE("BuildRequestBody: system prompt is prepended", "[openai_request]") {
    llm::ChatRequest request = BaseRequest();
    request.system = "you are a bot";

    nlohmann::json messages = BuildRequestBody(request)["messages"];

    REQUIRE(messages.size() == 2);
    CHECK(messages[0]["role"] == "system");
    CHECK(messages[0]["content"] == "you are a bot");
    CHECK(messages[1]["role"] == "user");
    CHECK(messages[1]["content"] == "hi");
}

TEST_CASE("ToOpenAiMessages: assistant message with one tool call", "[openai_request]") {
    llm::ChatRequest request;
    request.messages.push_back(
        llm::Message{llm::Role::Assistant, {llm::ToolUseBlock{"call_1", "read", {{"path", "a.txt"}}}}});

    nlohmann::json messages = ToOpenAiMessages(request);

    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["role"] == "assistant");
    CHECK(messages[0]["content"].is_null());
    REQUIRE(messages[0]["tool_calls"].size() == 1);
    CHECK(messages[0]["tool_calls"][0]["id"] == "call_1");
    CHECK(messages[0]["tool_calls"][0]["function"]["name"] == "read");
    CHECK(messages[0]["tool_calls"][0]["function"]["arguments"] == nlohmann::json({{"path", "a.txt"}}).dump());
}

TEST_CASE("ToOpenAiMessages: assistant message with multiple tool calls stays one message",
          "[openai_request]") {
    llm::ChatRequest request;
    request.messages.push_back(llm::Message{
        llm::Role::Assistant,
        {llm::ToolUseBlock{"call_1", "read", nlohmann::json::object()},
         llm::ToolUseBlock{"call_2", "write", nlohmann::json::object()}}});

    nlohmann::json messages = ToOpenAiMessages(request);

    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0]["tool_calls"].size() == 2);
    CHECK(messages[0]["tool_calls"][0]["id"] == "call_1");
    CHECK(messages[0]["tool_calls"][1]["id"] == "call_2");
}

TEST_CASE("ToOpenAiMessages: N tool results explode into N tool messages", "[openai_request]") {
    llm::ChatRequest request;
    llm::ToolResultBlock result1{"call_1", {llm::TextBlock{"result one"}}, false};
    llm::ToolResultBlock result2{"call_2", {llm::TextBlock{"result two"}}, false};
    request.messages.push_back(llm::Message{llm::Role::User, {result1, result2}});

    nlohmann::json messages = ToOpenAiMessages(request);

    REQUIRE(messages.size() == 2);
    CHECK(messages[0]["role"] == "tool");
    CHECK(messages[0]["tool_call_id"] == "call_1");
    CHECK(messages[0]["content"] == "result one");
    CHECK(messages[1]["role"] == "tool");
    CHECK(messages[1]["tool_call_id"] == "call_2");
    CHECK(messages[1]["content"] == "result two");
}

TEST_CASE("ToOpenAiMessages: ThinkingBlock is dropped silently", "[openai_request]") {
    llm::ChatRequest request;
    request.messages.push_back(
        llm::Message{llm::Role::Assistant, {llm::ThinkingBlock{"pondering", std::nullopt}, llm::TextBlock{"answer"}}});

    nlohmann::json messages = ToOpenAiMessages(request);

    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["content"] == "answer");
}

TEST_CASE("ToOpenAiToolChoice: all four kinds", "[openai_request]") {
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Auto, std::nullopt}) == "auto");
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::None, std::nullopt}) == "none");
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Required, std::nullopt}) == "required");

    nlohmann::json specific = ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Specific, "read"});
    CHECK(specific["type"] == "function");
    CHECK(specific["function"]["name"] == "read");
}

TEST_CASE("BuildRequestBody: tools/tool_choice omitted when there are no tools", "[openai_request]") {
    llm::ChatRequest request = BaseRequest();
    request.parameters = {{"tools", nlohmann::json::array({1})}, {"tool_choice", "auto"}};

    nlohmann::json body = BuildRequestBody(request);

    CHECK_FALSE(body.contains("tools"));
    CHECK_FALSE(body.contains("tool_choice"));
}

TEST_CASE("BuildRequestBody: tool order is preserved, never re-sorted", "[openai_request]") {
    llm::ChatRequest request = BaseRequest();
    request.tools = {{"zzz", "d1", nlohmann::json::object()}, {"aaa", "d2", nlohmann::json::object()}};

    nlohmann::json tools = BuildRequestBody(request)["tools"];

    REQUIRE(tools.size() == 2);
    CHECK(tools[0]["function"]["name"] == "zzz");
    CHECK(tools[1]["function"]["name"] == "aaa");
}

TEST_CASE("BuildRequestBody: stream_options.include_usage set iff streaming", "[openai_request]") {
    llm::ChatRequest streaming = BaseRequest();
    streaming.stream = true;
    CHECK(BuildRequestBody(streaming)["stream_options"]["include_usage"] == true);

    llm::ChatRequest non_streaming = BaseRequest();
    non_streaming.stream = false;
    CHECK_FALSE(BuildRequestBody(non_streaming).contains("stream_options"));
}

TEST_CASE("BuildRequestBody: max_tokens presence and precedence over parameters", "[openai_request]") {
    llm::ChatRequest with_max = BaseRequest();
    with_max.max_tokens = 256;
    with_max.parameters = {{"max_tokens", 1}};
    CHECK(BuildRequestBody(with_max)["max_tokens"] == 256);

    llm::ChatRequest without_max = BaseRequest();
    without_max.parameters = {{"max_tokens", 1}};
    CHECK_FALSE(BuildRequestBody(without_max).contains("max_tokens"));
}
