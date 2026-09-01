// The chat view's data plane: message-kind categorization, the command-notice
// routing, and the scrollback cursor. Pure -- the FTXUI rendering on top of
// this is deliberately not unit-tested (ARCHITECTURE.md "Testing").
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <type_traits>

#include "minimal/client/chat_model.hpp"

using namespace minimal;
using minimal::client::ChatRowKind;

namespace {

v1::StoredMessage MakeMessage(v1::Role role, std::vector<v1::ContentBlock> blocks) {
    v1::StoredMessage stored;
    stored.set_seq(0);
    stored.mutable_message()->set_role(role);
    for (auto& block : blocks) {
        *stored.mutable_message()->add_content() = std::move(block);
    }
    return stored;
}

v1::ContentBlock TextBlock(std::string text) {
    v1::ContentBlock block;
    block.mutable_text()->set_text(std::move(text));
    return block;
}

v1::ContentBlock ToolUse(const std::string& id, const std::string& name) {
    v1::ContentBlock block;
    block.mutable_tool_use()->set_id(id);
    block.mutable_tool_use()->set_name(name);
    return block;
}

v1::ContentBlock ToolResult(const std::string& id, const std::string& text, bool is_error) {
    v1::ContentBlock block;
    block.mutable_tool_result()->set_tool_use_id(id);
    block.mutable_tool_result()->set_is_error(is_error);
    block.mutable_tool_result()->add_content()->mutable_text()->set_text(text);
    return block;
}

}  // namespace

TEST_CASE("a plain conversation renders user text and agent markdown",
          "[chat_model]") {
    // Descending seq, as GetMessages returns: assistant LAST (seq 1), user
    // FIRST (seq 0). Display order is the reverse.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {TextBlock("**hello**")}));
    page.push_back(MakeMessage(v1::ROLE_USER, {TextBlock("hi there")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == ChatRowKind::UserText);
    // Verbatim, the markdown left alone: a user's typed underscores are not
    // emphasis. The VIEW chooses the styling; the row only records the kind.
    CHECK(rows[0].text == "hi there");
    CHECK(rows[1].kind == ChatRowKind::AgentMarkdown);
    CHECK(rows[1].text == "**hello**");
}

TEST_CASE("@-mention attachment blocks become attachment rows, text stays verbatim",
          "[chat_model][mention]") {
    // The E3 expansion shape: verbatim block 0, framed blocks after.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(
        v1::ROLE_USER,
        {TextBlock("see @a.md"), TextBlock("<file path=\"a.md\">\nA\n</file>"),
         TextBlock("<file-tree path=\"src/\">\nmain.cpp\n</file-tree>")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == ChatRowKind::UserText);
    CHECK(rows[1].kind == ChatRowKind::UserAttachment);
    CHECK(rows[1].attachment_path == "a.md");
    CHECK(rows[2].kind == ChatRowKind::UserAttachment);
    CHECK(rows[2].attachment_path == "src/");
}

TEST_CASE("tool calls become activity rows carrying the call's name and result body",
          "[chat_model]") {
    // Descending: the RESULT is newer than its use (the use ran first), so it
    // comes first in the page.
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "stdout tail", false)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 2);
    // Display order (oldest first): the use one-liner, then the result body.
    CHECK(rows[0].kind == ChatRowKind::ToolActivity);
    CHECK(rows[0].tool_name == "bash");
    CHECK(rows[0].text.empty());
    CHECK(rows[1].kind == ChatRowKind::ToolActivity);
    CHECK(rows[1].tool_name == "bash");  // the name rides on the USE, not the result
    CHECK(rows[1].text == "stdout tail");
    CHECK_FALSE(rows[1].tool_is_error);
}

TEST_CASE("an errored tool result is marked, not hidden", "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "boom", true)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "bash")}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 2);
    CHECK(rows[1].tool_is_error);
}

TEST_CASE("a subagent completion injection is a System row, not tool activity",
          "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_USER, {ToolResult("t1", "child text", false)}));
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT, {ToolUse("t1", "spawn_subagent")}));

    const auto rows = client::ChatRowsFromMessages(page);

    // The spawn CALL is still tool activity; the RESULT it eventually produced
    // is the harness speaking.
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == ChatRowKind::ToolActivity);
    CHECK(rows[0].tool_name == "spawn_subagent");
    CHECK(rows[1].kind == ChatRowKind::System);
    CHECK(rows[1].text == "child text");
    // The injection stays a REAL persisted message -- the model must see it at
    // the turn boundary (EXTENSIBILITY §4.4). Only its presentation is special.
}

TEST_CASE("thinking blocks become thinking rows", "[chat_model]") {
    std::vector<v1::StoredMessage> page;
    page.push_back(MakeMessage(v1::ROLE_ASSISTANT,
                               {[] {
                                    v1::ContentBlock b;
                                    b.mutable_thinking()->set_text("pondering");
                                    return b;
                                }()}));

    const auto rows = client::ChatRowsFromMessages(page);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ChatRowKind::Thinking);
    CHECK(rows[0].text == "pondering");
}

TEST_CASE("an empty page yields no rows", "[chat_model]") {
    CHECK(client::ChatRowsFromMessages({}).empty());
}

// --- command results: never chat -------------------------------------------

TEST_CASE("a command result becomes a notice with the right error flag",
          "[chat_model][command]") {
    v1::CommandResult ok;
    ok.set_status(v1::COMMAND_STATUS_OK);
    ok.set_message("agent: build");
    const auto ok_notice = client::NoticeFromCommandResult(ok);
    CHECK_FALSE(ok_notice.is_error);
    CHECK(ok_notice.message == "agent: build");

    v1::CommandResult bad;
    bad.set_status(v1::COMMAND_STATUS_ERROR);
    bad.set_message("usage: /agent <name>");
    CHECK(client::NoticeFromCommandResult(bad).is_error);
}

// The §4.4 invariant, expressed in the type system rather than by a check:
// a CommandNotice has NO conversion to the chat model's row type, so no future
// edit can "render a command result as a chat row" without defeating an
// explicit static_assert. Proven load-bearing by construction: adding such a
// conversion fails this compilation, which is the test.
static_assert(!std::is_convertible_v<client::CommandNotice, client::ChatRow>);
static_assert(!std::is_constructible_v<client::ChatRow, client::CommandNotice>);

// --- scrollback cursor ------------------------------------------------------

TEST_CASE("nothing loaded yet: fetch the newest page, cursorless", "[chat_model][scrollback]") {
    CHECK_FALSE(client::NextScrollbackCursor(-1, /*has_earlier=*/true).has_value());
}

TEST_CASE("history remains: the cursor is the oldest seq loaded", "[chat_model][scrollback]") {
    const auto cursor = client::NextScrollbackCursor(41, /*has_earlier=*/true);
    REQUIRE(cursor.has_value());
    CHECK(*cursor == 41);
}

TEST_CASE("history exhausted: no next page", "[chat_model][scrollback]") {
    CHECK_FALSE(client::NextScrollbackCursor(41, /*has_earlier=*/false).has_value());
}
