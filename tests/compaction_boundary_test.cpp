#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/compaction_boundary.hpp"

using namespace minimal::llm;
using minimal::agent::snap_compaction_boundary;
using minimal::agent::synthesize_missing_tool_results;

namespace {

Message UserText(std::string text) {
    return Message{Role::User, {ContentBlock{TextBlock{std::move(text)}}}};
}

Message AssistantToolUse(std::vector<std::pair<std::string, std::string>> id_name_pairs) {
    Message m{Role::Assistant, {}};
    for (auto& [id, name] : id_name_pairs) {
        m.content.push_back(ContentBlock{ToolUseBlock{id, name, nlohmann::json::object()}});
    }
    return m;
}

Message UserToolResult(std::string tool_use_id) {
    Message m{Role::User, {}};
    m.content.push_back(ContentBlock{ToolResultBlock{std::move(tool_use_id), {}, false}});
    return m;
}

}  // namespace

TEST_CASE("boundary at index 0 needs no snap", "[compaction]") {
    std::vector<Message> messages = {UserText("hi"), AssistantToolUse({{"t1", "read"}})};
    CHECK(snap_compaction_boundary(messages, 0) == 0);
}

TEST_CASE("an already-clean boundary is not moved", "[compaction]") {
    std::vector<Message> messages = {
        UserText("hi"), UserText("there"),
        AssistantToolUse({{"t1", "read"}}), UserToolResult("t1"),
    };
    CHECK(snap_compaction_boundary(messages, 2) == 2);
}

TEST_CASE("a boundary splitting one tool call snaps back before it", "[compaction]") {
    std::vector<Message> messages = {
        UserText("hi"),                       // 0
        AssistantToolUse({{"t1", "read"}}),   // 1
        UserToolResult("t1"),                 // 2
        UserText("thanks"),                   // 3
    };
    // Cutting at 2 would separate t1's tool_use (index 1) from its result (index 2).
    CHECK(snap_compaction_boundary(messages, 2) == 1);
}

TEST_CASE("multiple tool calls in one turn snap past all of them together", "[compaction]") {
    std::vector<Message> messages = {
        UserText("hi"),                                        // 0
        AssistantToolUse({{"t1", "read"}, {"t2", "read"}}),    // 1
        UserToolResult("t1"),                                   // 2
        UserToolResult("t2"),                                   // 3
    };
    // Cutting at 3 (between the two results) must snap all the way back to 1, not
    // stop at 2, since both results belong to the same assistant turn.
    CHECK(snap_compaction_boundary(messages, 3) == 1);
}

// --- resume repair: synthesize_missing_tool_results --------------------------
//
// A transcript torn mid-tool-sequence (cancel, or a crash between the assistant
// append and the results append) ends with tool_use blocks nothing answers.
// Replaying that is a hard 400; the repair makes it provider-valid again.

TEST_CASE("a provider-valid tail needs no repair", "[compaction][resume]") {
    SECTION("ends on a user message") {
        std::vector<Message> messages = {UserText("hi")};
        CHECK_FALSE(synthesize_missing_tool_results(messages).has_value());
    }
    SECTION("ends on an assistant message with no tool calls") {
        std::vector<Message> messages = {
            UserText("hi"),
            Message{Role::Assistant, {ContentBlock{TextBlock{"hello"}}}},
        };
        CHECK_FALSE(synthesize_missing_tool_results(messages).has_value());
    }
    SECTION("ends on a complete tool cycle") {
        std::vector<Message> messages = {
            UserText("hi"),
            AssistantToolUse({{"t1", "read"}}),
            UserToolResult("t1"),
        };
        CHECK_FALSE(synthesize_missing_tool_results(messages).has_value());
    }
    SECTION("empty history") {
        std::vector<Message> messages;
        CHECK_FALSE(synthesize_missing_tool_results(messages).has_value());
    }
}

TEST_CASE("a dangling tool_use gets a synthesized error result", "[compaction][resume]") {
    std::vector<Message> messages = {
        UserText("hi"),
        AssistantToolUse({{"t1", "bash"}}),
    };
    const auto repair = synthesize_missing_tool_results(messages);
    REQUIRE(repair.has_value());
    REQUIRE(repair->role == Role::User);
    REQUIRE(repair->content.size() == 1);
    const auto* result = std::get_if<ToolResultBlock>(&repair->content.front());
    REQUIRE(result != nullptr);
    CHECK(result->tool_use_id == "t1");
    CHECK(result->is_error);
    // The repair says what happened rather than inventing an outcome the tool
    // never produced.
    const auto* text = std::get_if<TextBlock>(&result->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text.find("interrupted") != std::string::npos);
}

TEST_CASE("every dangling tool_use of a batch is repaired", "[compaction][resume]") {
    std::vector<Message> messages = {
        UserText("hi"),
        AssistantToolUse({{"t1", "bash"}, {"t2", "read"}, {"t3", "bash"}}),
    };
    const auto repair = synthesize_missing_tool_results(messages);
    REQUIRE(repair.has_value());
    REQUIRE(repair->content.size() == 3);
    for (const auto& block : repair->content) {
        const auto* result = std::get_if<ToolResultBlock>(&block);
        REQUIRE(result != nullptr);
        CHECK(result->is_error);
    }
}

TEST_CASE("repair covers only the tool_results that are actually missing", "[compaction][resume]") {
    // The partial case: one result landed before the process died. Only the
    // unmatched calls get error results -- repairing an answered call would put
    // two results for one tool_use in the context, which is just as invalid.
    std::vector<Message> messages = {
        UserText("hi"),
        AssistantToolUse({{"t1", "bash"}, {"t2", "read"}}),
        UserToolResult("t1"),
    };
    const auto repair = synthesize_missing_tool_results(messages);
    REQUIRE(repair.has_value());
    REQUIRE(repair->content.size() == 1);
    const auto* result = std::get_if<ToolResultBlock>(&repair->content.front());
    REQUIRE(result != nullptr);
    CHECK(result->tool_use_id == "t2");
}

TEST_CASE("repair looks at the trailing assistant message only", "[compaction][resume]") {
    // An earlier orphaned cycle cannot exist in a transcript the loop wrote --
    // every tool round completed before the next message was appended -- so the
    // repair must not reach back past the tail and invent results for calls a
    // later message already answered.
    std::vector<Message> messages = {
        UserText("hi"),
        AssistantToolUse({{"t1", "bash"}}),
        UserToolResult("t1"),
        UserText("more"),
        Message{Role::Assistant, {ContentBlock{TextBlock{"done"}}}},
    };
    CHECK_FALSE(synthesize_missing_tool_results(messages).has_value());
}
