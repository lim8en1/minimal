#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/mcp/mcps_config.hpp"

using namespace minimal::config;

namespace {

nlohmann::json StdioEntry() {
    return {{"name", "github"},
            {"transport", "stdio"},
            {"command", "gh-mcp-server"},
            {"args", {"--token", "${GITHUB_TOKEN}"}},
            {"env", {{"LOG_LEVEL", "info"}}}};
}

nlohmann::json HttpEntry() {
    return {{"name", "remote"},
            {"transport", "http"},
            {"url", "https://mcp.example.com/sse"},
            {"headers", {{"Authorization", "Bearer ${MCP_TOKEN}"}}}};
}

}  // namespace

TEST_CASE("ParseMcpsConfig: stdio entry decodes every field", "[mcps_config]") {
    auto config = ParseMcpsConfig(nlohmann::json::array({StdioEntry()}));

    REQUIRE(config.entries.size() == 1);
    REQUIRE(config.errors.empty());
    const auto& entry = config.entries[0];
    CHECK(entry.name == "github");
    CHECK(entry.transport == McpTransport::Stdio);
    CHECK(entry.command == "gh-mcp-server");
    CHECK(entry.args == std::vector<std::string>{"--token", "${GITHUB_TOKEN}"});
    REQUIRE(entry.env.size() == 1);
    CHECK(entry.env.at("LOG_LEVEL") == "info");
    // Interpolation markers stay raw: resolving them is the MCP client's job.
    CHECK(entry.args[1] == "${GITHUB_TOKEN}");
    CHECK(entry.enabled);
}

TEST_CASE("ParseMcpsConfig: http entry decodes url and headers", "[mcps_config]") {
    auto config = ParseMcpsConfig(nlohmann::json::array({HttpEntry()}));

    REQUIRE(config.entries.size() == 1);
    REQUIRE(config.errors.empty());
    const auto& entry = config.entries[0];
    CHECK(entry.transport == McpTransport::Http);
    CHECK(entry.url == "https://mcp.example.com/sse");
    REQUIRE(entry.headers.size() == 1);
    CHECK(entry.headers.at("Authorization") == "Bearer ${MCP_TOKEN}");
    CHECK(entry.command.empty());
}

TEST_CASE("ParseMcpsConfig: enabled defaults to true and can be opted out of", "[mcps_config]") {
    auto entry = StdioEntry();
    entry["name"] = "github-off";
    entry["enabled"] = false;

    auto config = ParseMcpsConfig(nlohmann::json::array({StdioEntry(), entry}));
    REQUIRE(config.entries.size() == 2);
    CHECK(config.entries[0].enabled);
    CHECK_FALSE(config.entries[1].enabled);
}

TEST_CASE("ParseMcpsConfig: a bad entry degrades to an error, good ones survive", "[mcps_config]") {
    auto broken = StdioEntry();
    broken["name"] = "broken";
    broken.erase("command");  // stdio without a command

    auto config = ParseMcpsConfig(nlohmann::json::array({StdioEntry(), broken, HttpEntry()}));

    REQUIRE(config.entries.size() == 2);
    CHECK(config.Find("github") != nullptr);
    CHECK(config.Find("remote") != nullptr);
    CHECK(config.Find("broken") == nullptr);
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("broken") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: unknown transport is a per-entry error", "[mcps_config]") {
    auto entry = StdioEntry();
    entry["transport"] = "websocket";

    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    CHECK(config.entries.empty());
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("websocket") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: missing name falls back to the entry position in the error", "[mcps_config]") {
    auto entry = StdioEntry();
    entry.erase("name");

    auto config = ParseMcpsConfig(nlohmann::json::array({StdioEntry(), entry}));
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("#1") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a missing name is a named error, never a json exception",
          "[mcps_config][invariant]") {
    // entry.at("name") threw json.exception.out_of_range into the blanket
    // catch, so a missing key read like a library bug while every neighbouring
    // field had a hand-written message. Fails with the fix reverted: the error
    // text used to contain "json.exception".
    auto entry = StdioEntry();
    entry.erase("name");
    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("name") != std::string::npos);
    CHECK(config.errors[0].find("json.exception") == std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a missing transport is a named error, never a json exception",
          "[mcps_config][invariant]") {
    auto entry = HttpEntry();
    entry.erase("transport");
    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("transport") != std::string::npos);
    CHECK(config.errors[0].find("json.exception") == std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a missing command is a named error, never a json exception",
          "[mcps_config][invariant][regression]") {
    // entry.at("command") used to throw json.exception.out_of_range into the
    // blanket catch, unlike "name"/"transport" a few lines above which
    // already had this contains()+type check.
    auto entry = StdioEntry();
    entry.erase("command");
    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("command") != std::string::npos);
    CHECK(config.errors[0].find("json.exception") == std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a missing url is a named error, never a json exception",
          "[mcps_config][invariant][regression]") {
    auto entry = HttpEntry();
    entry.erase("url");
    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("url") != std::string::npos);
    CHECK(config.errors[0].find("json.exception") == std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a non-array 'args' is a named error, not a silently reordered list",
          "[mcps_config][regression]") {
    // The bug: entry.at("args") was iterated without an is_array() check.
    // nlohmann's range-for over a JSON OBJECT iterates its VALUES in
    // key-sorted order, so a typo'd object silently became an args list in
    // the wrong order instead of the intended rejection.
    auto entry = StdioEntry();
    entry["args"] = nlohmann::json{{"1", "--foo"}, {"0", "--bar"}};
    auto config = ParseMcpsConfig(nlohmann::json::array({entry}));
    REQUIRE(config.entries.empty());
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("args") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: duplicate name is a per-entry error", "[mcps_config]") {
    auto config = ParseMcpsConfig(nlohmann::json::array({StdioEntry(), StdioEntry()}));
    REQUIRE(config.entries.size() == 1);
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("duplicate") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: a CR/LF in an env or header value is a per-entry error",
          "[mcps_config]") {
    auto injected_env = StdioEntry();
    injected_env["name"] = "injected-env";
    injected_env["env"] = {{"LOG_LEVEL", "info\r\nX-Injected: evil"}};

    auto injected_header = HttpEntry();
    injected_header["name"] = "injected-header";
    injected_header["headers"] = {{"Authorization", "Bearer x\r\nX-Injected: evil"}};

    auto config = ParseMcpsConfig(nlohmann::json::array({injected_env, injected_header}));
    CHECK(config.entries.empty());
    REQUIRE(config.errors.size() == 2);
    CHECK(config.errors[0].find("injected-env") != std::string::npos);
    CHECK(config.errors[1].find("injected-header") != std::string::npos);
}

TEST_CASE("ParseMcpsConfig: non-array top level throws", "[mcps_config]") {
    CHECK_THROWS_AS(ParseMcpsConfig(nlohmann::json{{"name", "github"}}), std::runtime_error);
}

TEST_CASE("LoadMcpsConfig: missing file is the normal empty state, not an error", "[mcps_config]") {
    auto config = LoadMcpsConfig("/nonexistent/mcps.json");
    CHECK(config.entries.empty());
    CHECK(config.errors.empty());
}

TEST_CASE("LoadMcpsConfig: a present but malformed file throws", "[mcps_config]") {
    const auto path = std::filesystem::temp_directory_path() / "minimal_mcps_config_test.json";
    { std::ofstream(path) << "{ not json"; }

    CHECK_THROWS_AS(LoadMcpsConfig(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST_CASE("LoadMcpsConfig: reads and parses a real file", "[mcps_config]") {
    const auto path = std::filesystem::temp_directory_path() / "minimal_mcps_config_test.json";
    { std::ofstream(path) << nlohmann::json::array({StdioEntry(), HttpEntry()}).dump(); }

    auto config = LoadMcpsConfig(path);
    REQUIRE(config.entries.size() == 2);
    CHECK(config.Find("github") != nullptr);
    CHECK(config.Find("remote") != nullptr);

    std::filesystem::remove(path);
}
