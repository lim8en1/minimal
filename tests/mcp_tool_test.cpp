#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "minimal/mcp/mcp_tool.hpp"
#include "minimal/tools/tool_registry.hpp"

using minimal::llm::ToolDefinition;
using minimal::mcp::McpCallOutcome;
using minimal::mcp::McpInvoker;
using minimal::mcp::McpResultToToolResult;
using minimal::mcp::McpTool;
using minimal::tools::Tool;
using minimal::tools::ToolContext;
using minimal::tools::ToolRegistry;
using minimal::tools::ToolResult;

namespace {

class StubInvoker : public McpInvoker {
public:
    explicit StubInvoker(McpCallOutcome outcome) : outcome_(std::move(outcome)) {}

    McpCallOutcome CallTool(std::string_view, const nlohmann::json&,
                            const std::function<bool()>&) override {
        ++calls;
        return outcome_;
    }

    int calls = 0;

private:
    McpCallOutcome outcome_;
};

nlohmann::json TextResult(const std::string& text) {
    return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}})},
                          {"isError", false}};
}

McpCallOutcome OkOutcome(const std::string& text) {
    McpCallOutcome outcome;
    outcome.ok = true;
    outcome.result = TextResult(text);
    return outcome;
}

ToolDefinition Listing(const std::string& name) {
    ToolDefinition def;
    def.name = name;
    def.description = "d";
    def.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    return def;
}

}  // namespace

TEST_CASE("McpResultToToolResult: text content and isError map through", "[mcp_tool]") {
    const ToolResult ok = McpResultToToolResult(TextResult("hello"));
    CHECK_FALSE(ok.is_error);
    CHECK(ok.content_text == "hello");

    nlohmann::json failed = TextResult("boom");
    failed["isError"] = true;
    const ToolResult err = McpResultToToolResult(failed);
    CHECK(err.is_error);
    CHECK(err.content_text == "boom");
}

TEST_CASE("McpResultToToolResult: non-text content becomes a placeholder, not silence", "[mcp_tool]") {
    nlohmann::json result = {{"content", nlohmann::json::array({
                                                  nlohmann::json{{"type", "text"}, {"text", "before "}},
                                                  nlohmann::json{{"type", "image"}, {"data", "..."}},
                                              })},
                             {"isError", false}};
    const ToolResult converted = McpResultToToolResult(result);
    CHECK(converted.content_text == "before [image content omitted]");
}

TEST_CASE("McpResultToToolResult: error result with no content is an error, not an empty success", "[mcp_tool]") {
    const ToolResult converted = McpResultToToolResult(nlohmann::json{{"isError", true}});
    CHECK(converted.is_error);
    CHECK_FALSE(converted.content_text.empty());

    // A successful call with no content is legal (void-style tools): empty
    // text, not an error.
    const ToolResult empty_ok = McpResultToToolResult(nlohmann::json{{"content", nlohmann::json::array()},
                                                                      {"isError", false}});
    CHECK_FALSE(empty_ok.is_error);

    // Anything that is not an object cannot be trusted.
    CHECK(McpResultToToolResult(nlohmann::json("string")).is_error);
}

TEST_CASE("McpTool: spec is namespaced and ReadOnly, execute routes through the invoker", "[mcp_tool]") {
    auto invoker = std::make_shared<StubInvoker>(OkOutcome("done"));
    McpTool tool("ida", Listing("list_instances"), invoker);

    CHECK(tool.spec().name == "mcp__ida__list_instances");
    CHECK(tool.spec().category == minimal::tools::ToolCategory::ReadOnly);
    CHECK_FALSE(tool.path_confinement_supported());
    CHECK(tool.server_name() == "ida");
    CHECK(tool.bare_tool_name() == "list_instances");

    ToolContext ctx{minimal::common::DefaultLogger()};
    const ToolResult result = tool.execute(nlohmann::json::object(), ctx);
    CHECK(invoker->calls == 1);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "done");
}

TEST_CASE("McpTool: a failed call becomes an error ToolResult naming the reason", "[mcp_tool]") {
    McpCallOutcome failed;
    failed.ok = false;
    failed.error = "request timed out after 60000 ms";
    auto invoker = std::make_shared<StubInvoker>(failed);
    McpTool tool("ida", Listing("slow"), invoker);

    ToolContext ctx{minimal::common::DefaultLogger()};
    const ToolResult result = tool.execute(nlohmann::json::object(), ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("timed out") != std::string::npos);
}

TEST_CASE("McpTool: disconnected invoker degrades to an error result", "[mcp_tool]") {
    // No invoker at all -- the manager retired the connection. The tool object
    // itself stays registered until replacement, so a call must be an error,
    // never a crash.
    McpTool tool("dead", Listing("x"), nullptr);
    ToolContext ctx{minimal::common::DefaultLogger()};
    const ToolResult result = tool.execute(nlohmann::json::object(), ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("not connected") != std::string::npos);
}

TEST_CASE("ReplaceMcpTools swaps a server's set atomically and retires rather than frees", "[mcp_tool]") {
    ToolRegistry registry;
    auto first_invoker = std::make_shared<StubInvoker>(OkOutcome("v1"));
    auto second_invoker = std::make_shared<StubInvoker>(OkOutcome("v2"));

    std::vector<std::unique_ptr<Tool>> batch1;
    // Raw pointer held across the replacement -- the ToolExecutor pattern.
    McpTool* kept = nullptr;
    {
        auto tool = std::make_unique<McpTool>("ida", Listing("list_instances"), first_invoker);
        kept = tool.get();
        batch1.push_back(std::move(tool));
    }
    {
        auto tool = std::make_unique<McpTool>("ida", Listing("stop_instance"), first_invoker);
        batch1.push_back(std::move(tool));
    }
    registry.ReplaceMcpTools("ida", std::move(batch1));

    CHECK(registry.Find("mcp__ida__list_instances") == kept);
    REQUIRE(registry.Find("mcp__ida__stop_instance") != nullptr);

    // Re-discovery reports fewer tools: the stale one must disappear from the
    // map without being destroyed.
    std::vector<std::unique_ptr<Tool>> batch2;
    batch2.push_back(std::make_unique<McpTool>("ida", Listing("list_instances"), second_invoker));
    registry.ReplaceMcpTools("ida", std::move(batch2));

    CHECK(registry.Find("mcp__ida__stop_instance") == nullptr);
    REQUIRE(registry.Find("mcp__ida__list_instances") != nullptr);
    CHECK(registry.Find("mcp__ida__list_instances") != kept);  // new object

    // The retired object is still alive and callable -- no use-after-free.
    ToolContext ctx{minimal::common::DefaultLogger()};
    const ToolResult result = kept->execute(nlohmann::json::object(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "v1");
}

TEST_CASE("ReplaceMcpTools: first registration wins within a batch, foreign names refused", "[mcp_tool]") {
    ToolRegistry registry;
    auto invoker = std::make_shared<StubInvoker>(OkOutcome("x"));

    std::vector<std::unique_ptr<Tool>> batch;
    auto winner = std::make_unique<McpTool>("s", Listing("t"), invoker);
    Tool* winner_ptr = winner.get();
    batch.push_back(std::move(winner));
    batch.push_back(std::make_unique<McpTool>("s", Listing("t"), invoker));  // duplicate
    batch.push_back(std::make_unique<McpTool>("other", Listing("u"), invoker));  // wrong namespace
    registry.ReplaceMcpTools("s", std::move(batch));

    REQUIRE(registry.Find("mcp__s__t") == winner_ptr);
    CHECK(registry.Find("mcp__other__u") == nullptr);
}

TEST_CASE("ReplaceMcpTools: DefinitionsFor sees exactly the new set, sorted", "[mcp_tool]") {
    ToolRegistry registry;
    auto invoker = std::make_shared<StubInvoker>(OkOutcome("x"));

    std::vector<std::unique_ptr<Tool>> zeta_batch;
    zeta_batch.push_back(std::make_unique<McpTool>("zeta", Listing("b"), invoker));
    registry.ReplaceMcpTools("zeta", std::move(zeta_batch));

    std::vector<std::unique_ptr<Tool>> alpha_batch;
    alpha_batch.push_back(std::make_unique<McpTool>("alpha", Listing("a"), invoker));
    registry.ReplaceMcpTools("alpha", std::move(alpha_batch));

    const auto defs = registry.DefinitionsForMcpServers({"alpha", "zeta"});
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].name == "mcp__alpha__a");
    CHECK(defs[1].name == "mcp__zeta__b");
}
