#include <catch2/catch_test_macros.hpp>

#include <span>

#include "openai_request.hpp"

using namespace minimal;
using namespace minimal::provider_openai;

namespace {

// ChatRequest BORROWS its messages (see chat.hpp), so a test that builds one has
// to keep the vector alive alongside it rather than inside it. Pairing the two
// here is what stops every test from having to remember that. Non-copyable on
// purpose: a copy would leave the span pointing at the original's buffer.
struct Fixture {
    std::vector<llm::Message> messages;
    llm::ChatRequest request;

    explicit Fixture(std::vector<llm::Message> m) : messages(std::move(m)) {
        request.messages = messages;
    }
    // Copying would leave the span pointing at the original's buffer, so it is
    // gone. Moving is fine but not free: a moved vector keeps its heap buffer,
    // yet the span has to be re-pointed at the member that now owns it rather
    // than left aliasing a moved-from object.
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&& other) noexcept
        : messages(std::move(other.messages)), request(std::move(other.request)) {
        request.messages = messages;
    }
};

Fixture BaseRequest() {
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}}});
    fixture.request.model = "gpt-4o-mini";
    return fixture;
}

// The builder returns the serialized body (that is the point of it -- no DOM of
// the history); tests parse it to make structural assertions.
nlohmann::json ParseBody(const llm::ChatRequest& request) {
    return nlohmann::json::parse(BuildRequestBody(request));
}

}  // namespace

TEST_CASE("BuildRequestBody: structural fields always win over parameters", "[openai_request]") {
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.parameters = {{"model", "bogus"}, {"temperature", 0.7}, {"stream", true}};
    request.stream = false;

    const nlohmann::json body = ParseBody(request);

    CHECK(body["model"] == "gpt-4o-mini");
    CHECK(body["temperature"] == 0.7);  // untouched pass-through knob survives
    CHECK(body["stream"] == false);     // ChatRequest's own member wins
}

TEST_CASE("BuildRequestBody: colliding parameter keys are dropped, never emitted as duplicates",
          "[openai_request][invariant]") {
    // The old DOM builder let the later structural write overwrite the
    // parameter inside one object. A string builder appends in order, so the
    // guarantee has to come from DROPPING colliding keys -- emitting both would
    // leave precedence to whatever parser the backend uses.
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.parameters = {{"model", "bogus"},
                          {"messages", "bogus"},
                          {"stream", true},
                          {"tools", "bogus"},
                          {"tool_choice", "bogus"},
                          {"max_tokens", 1},
                          {"stream_options", "bogus"},
                          {"temperature", 0.7}};

    const std::string text = BuildRequestBody(request);

    // No duplicate keys: every structural name appears AT MOST once in the
    // serialized body (absence rules themselves are covered by the dedicated
    // tests below).
    for (const std::string_view key : {"model", "messages", "stream", "tools", "tool_choice",
                                       "max_tokens", "stream_options"}) {
        std::size_t occurrences = 0;
        for (std::size_t pos = text.find("\"" + std::string(key) + "\""); pos != std::string::npos;
             pos = text.find("\"" + std::string(key) + "\"", pos + 1)) {
            ++occurrences;
        }
        CHECK(occurrences <= 1);
    }

    const nlohmann::json body = nlohmann::json::parse(text);
    CHECK(body["model"] == "gpt-4o-mini");
    CHECK(body["temperature"] == 0.7);  // the non-structural knob survives
}

TEST_CASE("BuildRequestBody: system prompt is prepended", "[openai_request]") {
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.system = "you are a bot";

    const nlohmann::json messages = ParseBody(request)["messages"];

    REQUIRE(messages.size() == 2);
    CHECK(messages[0]["role"] == "system");
    CHECK(messages[0]["content"] == "you are a bot");
    CHECK(messages[1]["role"] == "user");
    CHECK(messages[1]["content"] == "hi");
}

TEST_CASE("BuildRequestBody: assistant message with one tool call", "[openai_request]") {
    Fixture fixture({llm::Message{llm::Role::Assistant,
                                  {llm::ToolUseBlock{"call_1", "read", {{"path", "a.txt"}}}}}});

    const nlohmann::json messages = ParseBody(fixture.request)["messages"];

    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["role"] == "assistant");
    CHECK(messages[0]["content"].is_null());
    REQUIRE(messages[0]["tool_calls"].size() == 1);
    CHECK(messages[0]["tool_calls"][0]["id"] == "call_1");
    CHECK(messages[0]["tool_calls"][0]["function"]["name"] == "read");
    CHECK(messages[0]["tool_calls"][0]["function"]["arguments"] == nlohmann::json({{"path", "a.txt"}}).dump());
}

TEST_CASE("BuildRequestBody: assistant message with multiple tool calls stays one message",
          "[openai_request]") {
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ToolUseBlock{"call_1", "read", nlohmann::json::object()},
         llm::ToolUseBlock{"call_2", "write", nlohmann::json::object()}}}});

    const nlohmann::json messages = ParseBody(fixture.request)["messages"];

    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0]["tool_calls"].size() == 2);
    CHECK(messages[0]["tool_calls"][0]["id"] == "call_1");
    CHECK(messages[0]["tool_calls"][1]["id"] == "call_2");
}

TEST_CASE("BuildRequestBody: multi-block user message concatenates into one content string",
          "[openai_request][mention]") {
    // The shape @-mention expansion produces (verbatim text + framed attachment
    // blocks). OpenAI chat-completions content is a plain string, so the blocks
    // concatenate -- harmless because the <file> framing is self-delimiting.
    // This is the re-proof that the flattening at openai_request.cpp's text join
    // is safe for exactly this message shape.
    Fixture fixture({llm::Message{
        llm::Role::User,
        {llm::TextBlock{"see @a.md and @b.md"},
         llm::TextBlock{"<file path=\"a.md\">\nA\n</file>"},
         llm::TextBlock{"<file path=\"b.md\">\nB\n</file>"}}}});

    const nlohmann::json messages = ParseBody(fixture.request)["messages"];

    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["role"] == "user");
    CHECK(messages[0]["content"] ==
          "see @a.md and @b.md<file path=\"a.md\">\nA\n</file><file path=\"b.md\">\nB\n</file>");
}

TEST_CASE("BuildRequestBody: N tool results explode into N tool messages", "[openai_request]") {
    llm::ToolResultBlock result1{"call_1", {llm::TextBlock{"result one"}}, false};
    llm::ToolResultBlock result2{"call_2", {llm::TextBlock{"result two"}}, false};
    Fixture fixture({llm::Message{llm::Role::User, {result1, result2}}});

    const nlohmann::json messages = ParseBody(fixture.request)["messages"];

    REQUIRE(messages.size() == 2);
    CHECK(messages[0]["role"] == "tool");
    CHECK(messages[0]["tool_call_id"] == "call_1");
    CHECK(messages[0]["content"] == "result one");
    CHECK(messages[1]["role"] == "tool");
    CHECK(messages[1]["tool_call_id"] == "call_2");
    CHECK(messages[1]["content"] == "result two");
}

TEST_CASE("BuildRequestBody: ThinkingBlock is dropped silently", "[openai_request]") {
    Fixture fixture({llm::Message{
        llm::Role::Assistant,
        {llm::ThinkingBlock{"pondering", std::nullopt}, llm::TextBlock{"answer"}}}});

    const nlohmann::json messages = ParseBody(fixture.request)["messages"];

    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["content"] == "answer");
}

TEST_CASE("BuildRequestBody: text that needs JSON escaping survives a round-trip",
          "[openai_request]") {
    // Hand-rolled serialization is where escaping bugs live: quotes, backslashes,
    // newlines, and unicode all pass through the string builder now.
    const std::string nasty = "quote \" backslash \\ newline \n tab \t unicode \xe2\x9c\x93";
    Fixture fixture({llm::Message{llm::Role::User, {llm::TextBlock{nasty}}}});
    fixture.request.system = "sys \" \\ \n";

    const nlohmann::json body = ParseBody(fixture.request);

    CHECK(body["messages"][0]["content"] == "sys \" \\ \n");  // the system prompt
    CHECK(body["messages"][1]["content"] == nasty);
}

TEST_CASE("ToOpenAiToolChoice: all four kinds", "[openai_request]") {
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Auto, std::nullopt}) == "auto");
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::None, std::nullopt}) == "none");
    CHECK(ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Required, std::nullopt}) == "required");

    const nlohmann::json specific = ToOpenAiToolChoice(llm::ToolChoiceSpec{llm::ToolChoiceKind::Specific, "read"});
    CHECK(specific["type"] == "function");
    CHECK(specific["function"]["name"] == "read");
}

TEST_CASE("BuildRequestBody: tools/tool_choice omitted when there are no tools", "[openai_request]") {
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.parameters = {{"tools", nlohmann::json::array({1})}, {"tool_choice", "auto"}};

    const nlohmann::json body = ParseBody(request);

    CHECK_FALSE(body.contains("tools"));
    CHECK_FALSE(body.contains("tool_choice"));
}

TEST_CASE("BuildRequestBody: tool order is preserved, never re-sorted", "[openai_request]") {
    Fixture fixture = BaseRequest();
    auto& request = fixture.request;
    request.tools = {{"zzz", "d1", nlohmann::json::object()}, {"aaa", "d2", nlohmann::json::object()}};

    const nlohmann::json tools = ParseBody(request)["tools"];

    REQUIRE(tools.size() == 2);
    CHECK(tools[0]["function"]["name"] == "zzz");
    CHECK(tools[1]["function"]["name"] == "aaa");
}

TEST_CASE("BuildRequestBody: stream_options.include_usage set iff streaming", "[openai_request]") {
    Fixture streaming_fixture = BaseRequest();
    auto& streaming = streaming_fixture.request;
    streaming.stream = true;
    CHECK(ParseBody(streaming)["stream_options"]["include_usage"] == true);

    Fixture non_streaming_fixture = BaseRequest();
    auto& non_streaming = non_streaming_fixture.request;
    non_streaming.stream = false;
    CHECK_FALSE(ParseBody(non_streaming).contains("stream_options"));
}

TEST_CASE("BuildRequestBody: max_tokens presence and precedence over parameters", "[openai_request]") {
    Fixture with_max_fixture = BaseRequest();
    auto& with_max = with_max_fixture.request;
    with_max.max_tokens = 256;
    with_max.parameters = {{"max_tokens", 1}};
    CHECK(ParseBody(with_max)["max_tokens"] == 256);

    Fixture without_max_fixture = BaseRequest();
    auto& without_max = without_max_fixture.request;
    without_max.parameters = {{"max_tokens", 1}};
    CHECK_FALSE(ParseBody(without_max).contains("max_tokens"));
}
