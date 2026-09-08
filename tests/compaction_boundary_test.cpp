#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/compaction_boundary.hpp"

using namespace minimal::llm;
using minimal::agent::snap_compaction_boundary;

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

// Thinking rides INSIDE its assistant message, so a boundary snap that moves
// the tool_use necessarily moves the thinking with it -- this helper exists to
// pin that no cut can orphan thinking from the tool call it reasoned about.
Message AssistantThinkingThenToolUse(std::string thinking, std::string id, std::string name) {
    Message m{Role::Assistant, {}};
    m.content.push_back(
        ContentBlock{ThinkingBlock{std::move(thinking), std::nullopt, std::nullopt}});
    m.content.push_back(ContentBlock{ToolUseBlock{std::move(id), std::move(name),
                                                  nlohmann::json::object()}});
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

TEST_CASE("a snap never separates thinking from the tool call it precedes", "[compaction]") {
    // Extended thinking is the first block of the assistant message that also
    // carries the tool_use. Cutting between them is impossible at message
    // granularity -- this pins that the snap granularity stays the MESSAGE,
    // not the block, as replayable-thinking support lands.
    std::vector<Message> messages = {
        UserText("hi"),                                        // 0
        AssistantThinkingThenToolUse("why", "t1", "read"),     // 1
        UserToolResult("t1"),                                   // 2
        UserText("thanks"),                                     // 3
    };
    CHECK(snap_compaction_boundary(messages, 2) == 1);
    CHECK(snap_compaction_boundary(messages, 3) == 3);
}
