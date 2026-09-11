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

TEST_CASE("BuildRequestBody: a tool_result's display payload never reaches the model",
         "[anthropic_request][invariant]") {
    llm::ToolResultBlock result;
    result.tool_use_id = "toolu_1";
    result.content = {llm::TextBlock{"replaced 1 occurrence"}};
    result.display = {{"kind", "diff"}, {"path", "DISPLAY_SENTINEL"}};
    Fixture fixture({llm::Message{llm::Role::User, {result}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    CHECK(body.dump().find("DISPLAY_SENTINEL") == std::string::npos);
    CHECK_FALSE(body["messages"][0]["content"][0].contains("display"));
}

TEST_CASE("BuildRequestBody: unsigned thinking blocks are dropped on replay",
          "[anthropic_request]") {
    // An unsigned block cannot be replayed (the API rejects or silently
    // mishandles a resynthesized signature), so it stays dropped.
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ThinkingBlock{"reasoning...", std::nullopt}, llm::TextBlock{"answer"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"][0]["content"].size() == 1);
    CHECK(body["messages"][0]["content"][0]["type"] == "text");
}

TEST_CASE("BuildRequestBody: signed thinking blocks replay with their signature",
          "[anthropic_request]") {
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ThinkingBlock{"reasoning...", std::string("sig-abc"), std::nullopt},
         llm::TextBlock{"answer"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"][0]["content"].size() == 2);
    const auto& block = body["messages"][0]["content"][0];
    CHECK(block["type"] == "thinking");
    CHECK(block["thinking"] == "reasoning...");
    CHECK(block["signature"] == "sig-abc");
    // Thinking precedes the message's own text, as the API requires.
    CHECK(body["messages"][0]["content"][1]["type"] == "text");
}

TEST_CASE("BuildRequestBody: redacted thinking replays its data verbatim",
          "[anthropic_request]") {
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ThinkingBlock{"", std::nullopt, std::string("encrypted-blob")},
         llm::TextBlock{"answer"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"][0]["content"].size() == 2);
    const auto& block = body["messages"][0]["content"][0];
    CHECK(block["type"] == "redacted_thinking");
    CHECK(block["data"] == "encrypted-blob");
    CHECK_FALSE(block.contains("thinking"));
    CHECK_FALSE(block.contains("signature"));
}

TEST_CASE("BuildRequestBody: invalid UTF-8 is replaced with U+FFFD, never thrown",
          "[anthropic_request]") {
    // Tool output reaches message text unsanitized; dump()'s default error
    // handler used to throw type_error.316 on a binary tool result, and the
    // provider's build call was outside its try/catch. The escaping now
    // replaces invalid sequences instead of throwing.
    const std::string binary = "bad \xff\xfe bytes";
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{binary}}}});
    fixture.request.system = "sys \xff";
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    nlohmann::json body;
    REQUIRE_NOTHROW(body = ParseBody(fixture.request));

    const std::string replacement = "\xEF\xBF\xBD";  // U+FFFD
    CHECK(body["system"][0]["text"] == "sys " + replacement);
    CHECK(body["messages"][0]["content"][0]["text"] ==
          "bad " + replacement + replacement + " bytes");
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

TEST_CASE("BuildRequestBody: a message whose content is entirely dropped is skipped",
          "[anthropic_request][invariant]") {
    // An assistant turn consisting only of UNSIGNED thinking blocks (not
    // replayable, so dropped) would otherwise serialize as "content":[] --
    // which the API rejects outright.
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}},
                     llm::Message{llm::Role::Assistant, {llm::ThinkingBlock{"hmm", std::nullopt}}},
                     llm::Message{llm::Role::User, {llm::TextBlock{"continue"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"].is_array());
    // The empty assistant message vanishes; the two user messages merge.
    REQUIRE(body["messages"].size() == 1);
    CHECK(body["messages"][0]["role"] == "user");
    CHECK(body["messages"][0]["content"].size() == 2);
}

TEST_CASE("BuildRequestBody: a signed thinking-only assistant turn renders as its own message",
          "[anthropic_request]") {
    // Replayable content must NOT be skipped -- dropping it would break the
    // extended-thinking tool loop the signature exists to serve.
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}},
                     llm::Message{llm::Role::Assistant,
                                  {llm::ThinkingBlock{"hmm", std::string("sig"), std::nullopt}}},
                     llm::Message{llm::Role::User, {llm::TextBlock{"continue"}}}});
    fixture.request.model = "claude-opus-5";
    fixture.request.max_tokens = 1024;

    const nlohmann::json body = ParseBody(fixture.request);
    REQUIRE(body["messages"].size() == 3);
    CHECK(body["messages"][1]["role"] == "assistant");
    REQUIRE(body["messages"][1]["content"].size() == 1);
    CHECK(body["messages"][1]["content"][0]["type"] == "thinking");
    CHECK(body["messages"][1]["content"][0]["signature"] == "sig");
}

TEST_CASE("BuildRequestBody: cache_ttl rejects values the API would refuse",
          "[anthropic_request]") {
    Fixture fixture = BaseRequest();
    fixture.request.system = "sys";
    fixture.request.parameters = {{"cache_ttl", "7d"}};
    CHECK_THROWS_AS(BuildRequestBody(fixture.request, false), std::invalid_argument);

    Fixture ok = BaseRequest();
    ok.request.system = "sys";
    ok.request.parameters = {{"cache_ttl", "1h"}};
    CHECK_NOTHROW(BuildRequestBody(ok.request, false));
}
