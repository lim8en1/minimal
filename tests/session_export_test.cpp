// session-export (compiled in directly, the bundled-plugin pattern): the
// renderer's filters and start points, the paged read back into one ascending
// transcript, and where a typed path lands.

#include <cstdlib>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "export_render.hpp"
#include "export_view.hpp"
#include "transcript_fetch.hpp"

using namespace minimal::session_export;
namespace v1 = minimal::v1;
namespace plugins = minimal::client::plugins;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

v1::StoredMessage UserText(std::int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_USER);
    stored.mutable_message()->add_content()->mutable_text()->set_text(text);
    return stored;
}

// An assistant turn: thinking, text, then a tool call.
v1::StoredMessage AssistantTurn(std::int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.set_model("local/test-model");
    auto* message = stored.mutable_message();
    message->set_role(v1::ROLE_ASSISTANT);
    message->add_content()->mutable_thinking()->set_text("pondering " + text);
    message->add_content()->mutable_text()->set_text(text);
    auto* use = message->add_content()->mutable_tool_use();
    use->set_id("t" + std::to_string(seq));
    use->set_name("bash");
    (*use->mutable_input()->mutable_fields())["command"].set_string_value("ls");
    return stored;
}

// The user turn carrying only a tool result.
v1::StoredMessage ToolResult(std::int64_t seq, const std::string& output) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_USER);
    auto* result = stored.mutable_message()->add_content()->mutable_tool_result();
    result->set_tool_use_id("t" + std::to_string(seq - 1));
    result->add_content()->mutable_text()->set_text(output);
    return stored;
}

v1::CompactionEvent Compaction(std::int64_t at, std::int64_t boundary, const std::string& summary,
                               bool skipped = false) {
    v1::CompactionEvent event;
    event.set_at_seq(at);
    event.set_boundary_seq(boundary);
    event.set_folded_messages(boundary + 1);
    event.set_summary(summary);
    event.set_trigger("manual");
    event.set_skipped(skipped);
    return event;
}

Transcript Sample() {
    Transcript transcript;
    transcript.messages = {UserText(0, "first question"), AssistantTurn(1, "first answer"),
                           ToolResult(2, "file-a"), UserText(3, "second question"),
                           AssistantTurn(4, "second answer"), ToolResult(5, "file-b")};
    transcript.compactions = {Compaction(2, 1, "skipped one", /*skipped=*/true),
                              Compaction(3, 2, "## Summary\nthe first exchange, folded")};
    return transcript;
}

}  // namespace

TEST_CASE("session-export: thinking and tool calls follow their checkboxes",
          "[session_export]") {
    const Transcript transcript = Sample();
    ExportOptions options;
    options.session_id = "abc";

    options.thinking = false;
    options.tool_calls = true;
    std::string doc = Render(transcript, options);
    CHECK(Contains(doc, "# Session abc"));
    CHECK(Contains(doc, "first answer"));
    CHECK_FALSE(Contains(doc, "pondering"));
    CHECK(Contains(doc, "Tool call: bash"));
    CHECK(Contains(doc, "\"command\": \"ls\""));
    CHECK(Contains(doc, "file-a"));
    CHECK(Contains(doc, "local/test-model"));
    CHECK(Contains(doc, "## Tool\n"));  // a tool-results-only turn is not the user's

    options.thinking = true;
    options.tool_calls = false;
    doc = Render(transcript, options);
    CHECK(Contains(doc, "pondering first answer"));
    CHECK_FALSE(Contains(doc, "Tool call"));
    CHECK_FALSE(Contains(doc, "file-a"));
    // The tool-result-only user turns had nothing else: gone, heading and all.
    CHECK(CountExported(transcript, options) == 4);
}

TEST_CASE("session-export: a compaction start writes its summary, then what followed it",
          "[session_export]") {
    const Transcript transcript = Sample();
    ExportOptions options;
    options.compaction = 1;  // the real one; boundary_seq 2
    const std::string doc = Render(transcript, options);
    CHECK(Contains(doc, "Summary (compacted"));
    CHECK(Contains(doc, "the first exchange, folded"));
    CHECK_FALSE(Contains(doc, "first question"));
    CHECK_FALSE(Contains(doc, "file-a"));
    CHECK(Contains(doc, "second question"));
    CHECK(doc.find("the first exchange") < doc.find("second question"));
    CHECK(CountExported(transcript, options) == 3);
}

TEST_CASE("session-export: start points skip compactions that folded nothing",
          "[session_export]") {
    const auto points = BuildStartPoints(Sample());
    REQUIRE(points.size() == 2);
    CHECK_FALSE(points[0].compaction.has_value());
    CHECK(Contains(points[0].label, "from start"));
    CHECK(Contains(points[0].label, "first question"));
    REQUIRE(points[1].compaction.has_value());
    CHECK(*points[1].compaction == 1);
    CHECK(Contains(points[1].label, "manual"));
    // The preview is the summary's first real line, markup stripped.
    CHECK(Contains(points[1].label, "Summary"));
    CHECK_FALSE(Contains(points[1].label, "##"));
}

TEST_CASE("session-export: .txt is plain text with the same content", "[session_export]") {
    ExportOptions options;
    options.thinking = true;
    options.plain_text = true;
    options.session_id = "abc";
    const std::string doc = Render(Sample(), options);
    CHECK(Contains(doc, "=== User"));
    CHECK(Contains(doc, "[thinking]"));
    CHECK(Contains(doc, "[Tool call: bash]"));
    CHECK(Contains(doc, "    file-a"));
    CHECK_FALSE(Contains(doc, "```"));
    CHECK_FALSE(Contains(doc, "## "));
    CHECK_FALSE(Contains(doc, "**"));

    CHECK(IsPlainText("/tmp/x.TXT"));
    CHECK_FALSE(IsPlainText("/tmp/x.md"));
    CHECK_FALSE(IsPlainText("/tmp/x"));
}

TEST_CASE("session-export: a fence outgrows the backticks inside it", "[session_export]") {
    Transcript transcript;
    transcript.messages = {ToolResult(1, "has ``` inside and ```` too")};
    const std::string doc = Render(transcript, ExportOptions{});
    CHECK(Contains(doc, "`````\nhas ``` inside"));
}

namespace {

// A transcript served in pages of `page_size`, newest first, each with the
// compactions whose at_seq falls inside it -- the server's contract.
class PagedApi final : public plugins::ClientApi {
public:
    Transcript whole;
    int page_size = 2;
    int calls = 0;
    int fail_on_call = -1;

    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t> before,
                                           int) override {
        if (calls++ == fail_on_call) return std::nullopt;
        MessagePage page;
        for (auto it = whole.messages.rbegin(); it != whole.messages.rend(); ++it) {
            if (before && it->seq() >= *before) continue;
            if (static_cast<int>(page.messages.size()) == page_size) {
                page.has_more = true;
                break;
            }
            page.messages.push_back(*it);
        }
        for (const auto& compaction : whole.compactions) {
            for (const auto& stored : page.messages) {
                if (stored.seq() == compaction.at_seq()) page.compactions.push_back(compaction);
            }
        }
        return page;
    }

    std::optional<std::string> PluginCall(const std::string&, const std::string&,
                                          const std::string&) override {
        return std::nullopt;
    }
    std::optional<v1::ListModelsResponse> ListModels(bool, std::string*) override {
        return std::nullopt;
    }
    std::optional<v1::ListAgentPresetsResponse> ListAgentPresets(std::string*) override {
        return std::nullopt;
    }
    std::optional<v1::ListSessionsResponse> ListSessions(const SessionQuery&,
                                                         std::string*) override {
        return std::nullopt;
    }
    std::optional<std::vector<v1::SessionSummary>> ListSubagents(const std::string&,
                                                                 std::string*) override {
        return std::nullopt;
    }
    bool DeleteSession(const std::string&, std::string*) override { return false; }
    minimal::client::SubscribeOutcome PluginSubscribe(
        const std::string&, const std::string&, const std::string&,
        const std::function<bool(const std::string&)>&) override {
        return minimal::client::SubscribeOutcome::kError;
    }
};

}  // namespace

TEST_CASE("session-export: the paged read comes back whole and ascending", "[session_export]") {
    PagedApi api;
    api.whole = Sample();
    const auto transcript = FetchTranscript(api, "abc");
    REQUIRE(transcript.has_value());
    CHECK(api.calls == 3);
    REQUIRE(transcript->messages.size() == 6);
    for (std::size_t i = 0; i < transcript->messages.size(); ++i) {
        CHECK(transcript->messages[i].seq() == static_cast<std::int64_t>(i));
    }
    REQUIRE(transcript->compactions.size() == 2);
    CHECK(transcript->compactions[0].at_seq() == 2);
    CHECK(transcript->compactions[1].at_seq() == 3);
}

TEST_CASE("session-export: a failed page fails the whole read", "[session_export]") {
    PagedApi api;
    api.whole = Sample();
    api.fail_on_call = 1;
    CHECK_FALSE(FetchTranscript(api, "abc").has_value());
}

TEST_CASE("session-export: where a typed path lands", "[session_export]") {
    const std::filesystem::path start = "/work/project";
    CHECK(ResolvePath("  ", start).empty());
    CHECK(ResolvePath("out.md", start) == "/work/project/out.md");
    CHECK(ResolvePath(" ../out.md ", start) == "/work/out.md");
    CHECK(ResolvePath("/abs/out.txt", start) == "/abs/out.txt");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        CHECK(ResolvePath("~/out.md", start) ==
              (std::filesystem::path(home) / "out.md").lexically_normal());
    }

    const auto path = DefaultPath(start, "sess-1", 0);
    CHECK(path.parent_path() == start);
    CHECK(path.filename().string().starts_with("minimal-sess-1-"));
    CHECK(path.extension() == ".md");
}
