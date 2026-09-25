#include <catch2/catch_test_macros.hpp>

#include "mcp_picker_model.hpp"

using namespace minimal::mcp_view;

TEST_CASE("mcp-view: the endpoint's list decodes; other shapes do not", "[mcp_view]") {
    const auto servers = ParseServerList(
        R"({"servers":[{"name":"b","transport":"http","enabled":true,"alive":true,"error":""},)"
        R"({"name":"a","transport":"stdio","enabled":false},{"transport":"stdio"}]})");
    REQUIRE(servers.has_value());
    REQUIRE(servers->size() == 2);  // the nameless entry is skipped
    CHECK((*servers)[0].name == "b");
    CHECK((*servers)[0].alive);

    CHECK_FALSE(ParseServerList("[]").has_value());
    CHECK_FALSE(ParseServerList("not json").has_value());
}

TEST_CASE("mcp-view: rows are sorted, filtered, and say the live status", "[mcp_view]") {
    std::vector<McpServerStatus> servers = {
        {"zeta", "stdio", true, false, "spawn failed"},
        {"alpha", "http", false, false, ""},
        {"Beta", "stdio", true, true, ""},
        {"gamma", "stdio", true, false, ""},
    };
    const auto rows = BuildMcpPickerRows(servers, "");
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].label == "Beta  [stdio, enabled, connected]");
    CHECK(rows[1].label == "alpha  [http, disabled]");
    CHECK(rows[2].label == "gamma  [stdio, enabled, not yet connected]");
    CHECK(rows[3].label == "zeta  [stdio, enabled, error: spawn failed]");

    const auto filtered = BuildMcpPickerRows(servers, "BET");
    REQUIRE(filtered.size() == 1);
    CHECK(filtered[0].name == "Beta");
}
