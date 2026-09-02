#include <catch2/catch_test_macros.hpp>

#include "anthropic_request.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;

namespace {

// ChatRequest BORROWS its messages (see chat.hpp) -- same fixture shape as
// provider-openai's tests, for the same reason: a copy would leave the span
// pointing at the original's buffer.
struct Fixture {
    std::vector<llm::Message> messages;
    llm::ChatRequest request;

    explicit Fixture(std::vector<llm::Message> m) : messages(std::move(m)) {
        request.messages = messages;
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&& other) noexcept
        : messages(std::move(other.messages)), request(std::move(other.request)) {
        request.messages = messages;
    }
};

Fixture BaseRequest() {
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;
    return fixture;
}

nlohmann::json ParseBody(const llm::ChatRequest& request) {
    return nlohmann::json::parse(BuildRequestBody(request, /*stream=*/false));
}

}  // namespace

TEST_CASE("BuildRequestBody: max_tokens is required", "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    fixture.request.max_tokens.reset();
    CHECK_THROWS_AS(BuildRequestBody(fixture.request, false), std::invalid_argument);
}

TEST_CASE("BuildRequestBody: structural fields always win over parameters", "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.parameters = {{"model", "bogus"}, {"temperature", 0.7}, {"stream", true}};
    request.stream = false;

    const nlohmann::json body = ParseBody(request);

    CHECK(body["model"] == "claude-opus-5");
    CHECK(body["temperature"] == 0.7);  // untouched pass-through knob survives
    CHECK(body["stream"] == false);     // the `stream` PARAMETER passed to BuildRequestBody wins
}

TEST_CASE("BuildRequestBody: system is a one-element block array with a cache breakpoint",
         "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    fixture.request.system = "you are terse";

    const nlohmann::json body = ParseBody(fixture.request);

    REQUIRE(body["system"].is_array());
    REQUIRE(body["system"].size() == 1);
    CHECK(body["system"][0]["type"] == "text");
    CHECK(body["system"][0]["text"] == "you are terse");
    CHECK(body["system"][0]["cache_control"]["type"] == "ephemeral");
    CHECK_FALSE(body["system"][0]["cache_control"].contains("ttl"));   // default 5-minute TTL
}

TEST_CASE("BuildRequestBody: no system field at all when ChatRequest::system is unset",
         "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    const nlohmann::json body = ParseBody(fixture.request);
    CHECK_FALSE(body.contains("system"));
}

TEST_CASE("BuildRequestBody: cache_ttl in parameters sets the system block's ttl and is "
         "consumed, not passed through",
         "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    fixture.request.system = "sys";
    fixture.request.parameters = {{"cache_ttl", "1h"}};

    const nlohmann::json body = ParseBody(fixture.request);

    CHECK(body["system"][0]["cache_control"]["ttl"] == "1h");
    CHECK_FALSE(body.contains("cache_ttl"));   // never a real top-level Anthropic field
}

TEST_CASE("BuildRequestBody: top-level automatic cache_control is always attached",
         "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    const nlohmann::json body = ParseBody(fixture.request);
    CHECK(body["cache_control"]["type"] == "ephemeral");
}

TEST_CASE("BuildRequestBody: consecutive same-role messages are merged into one wire message",
         "[anthropic_request]") {
    // Anthropic enforces strict user/assistant alternation; two adjacent User
    // llm::Messages (e.g. a compaction summary followed by a kept message)
    // must become ONE wire message with a two-item content array, not two
    // consecutive "role":"user" messages the API would refuse.
    Fixture fixture({
        llm::Message{llm::Role::User, {llm::TextBlock{"first"}}},
        llm::Message{llm::Role::User, {llm::TextBlock{"second"}}},
        llm::Message{llm::Role::Assistant, {llm::TextBlock{"reply"}}},
    });
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    const auto& messages = body["messages"];

    REQUIRE(messages.size() == 2);   // merged user pair, then the assistant reply
    CHECK(messages[0]["role"] == "user");
    REQUIRE(messages[0]["content"].size() == 2);
    CHECK(messages[0]["content"][0]["text"] == "first");
    CHECK(messages[0]["content"][1]["text"] == "second");
    CHECK(messages[1]["role"] == "assistant");
    CHECK(messages[1]["content"][0]["text"] == "reply");
}

TEST_CASE("BuildRequestBody: tool_use content carries a nested JSON object input, not a string",
         "[anthropic_request]") {
    llm::ToolUseBlock tool_use;
    tool_use.id = "toolu_1";
    tool_use.name = "read";
    tool_use.input = {{"path", "a.txt"}};
    Fixture fixture({llm::Message{llm::Role::Assistant, {tool_use}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    const auto& block = body["messages"][0]["content"][0];
    CHECK(block["type"] == "tool_use");
    CHECK(block["id"] == "toolu_1");
    CHECK(block["input"]["path"] == "a.txt");   // an object, not a stringified one
}

TEST_CASE("BuildRequestBody: tool_result carries is_error and joined text content",
         "[anthropic_request]") {
    llm::ToolResultBlock result;
    result.tool_use_id = "toolu_1";
    result.content = {llm::TextBlock{"boom"}};
    result.is_error = true;
    Fixture fixture({llm::Message{llm::Role::User, {result}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    const auto& block = body["messages"][0]["content"][0];
    CHECK(block["type"] == "tool_result");
    CHECK(block["tool_use_id"] == "toolu_1");
    CHECK(block["content"] == "boom");
    CHECK(block["is_error"] == true);
}

TEST_CASE("BuildRequestBody: thinking blocks are dropped on replay", "[anthropic_request]") {
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ThinkingBlock{"reasoning...", std::nullopt}, llm::TextBlock{"answer"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"][0]["content"].size() == 1);
    CHECK(body["messages"][0]["content"][0]["type"] == "text");
}

TEST_CASE("ToAnthropicTools: no function wrapper, matches ToolDefinition directly",
         "[anthropic_request]") {
    llm::ToolDefinition tool;
    tool.name = "read";
    tool.description = "reads a file";
    tool.input_schema = {{"type", "object"}};

    const nlohmann::json out = ToAnthropicTools({tool});
    REQUIRE(out.size() == 1);
    CHECK(out[0]["name"] == "read");
    CHECK(out[0]["description"] == "reads a file");
    CHECK(out[0]["input_schema"]["type"] == "object");
    CHECK_FALSE(out[0].contains("type"));   // no {"type":"function","function":{...}} wrapper
}

TEST_CASE("ToAnthropicToolChoice: kind mapping", "[anthropic_request]") {
    CHECK(ToAnthropicToolChoice({llm::ToolChoiceKind::Auto, std::nullopt})["type"] == "auto");
    CHECK(ToAnthropicToolChoice({llm::ToolChoiceKind::None, std::nullopt})["type"] == "none");
    CHECK(ToAnthropicToolChoice({llm::ToolChoiceKind::Required, std::nullopt})["type"] == "any");

    llm::ToolChoiceSpec specific{llm::ToolChoiceKind::Specific, std::string("read")};
    const auto out = ToAnthropicToolChoice(specific);
    CHECK(out["type"] == "tool");
    CHECK(out["name"] == "read");
}

TEST_CASE("ToAnthropicToolChoice: Specific without a tool_name throws", "[anthropic_request]") {
    llm::ToolChoiceSpec specific{llm::ToolChoiceKind::Specific, std::nullopt};
    CHECK_THROWS_AS(ToAnthropicToolChoice(specific), std::invalid_argument);
}
