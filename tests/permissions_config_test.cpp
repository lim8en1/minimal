#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/config/permissions_config.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;

TEST_CASE("ParsePermissionsConfig: empty doc yields documented defaults", "[permissions_config]") {
    auto config = ParsePermissionsConfig(nlohmann::json::object());

    CHECK(config.default_run_mode == PermissionRunMode::Normal);
    CHECK(config.mcp_default_mode == PermissionMode::Ask);
    CHECK_FALSE(config.global_layer.read_only.has_value());
    CHECK_FALSE(config.global_layer.file_write.has_value());
    CHECK_FALSE(config.global_layer.execute.has_value());
    CHECK_FALSE(config.global_layer.out_of_workdir.has_value());
    CHECK(config.global_layer.per_tool_overrides.empty());
}

TEST_CASE("ParsePermissionsConfig: fallback.* lands in global_layer", "[permissions_config]") {
    nlohmann::json doc = {{"fallback",
                            {{"read_only", "allow"},
                             {"file_write", "deny"},
                             {"execute", "ask"},
                             {"out_of_workdir", "deny"}}}};

    auto config = ParsePermissionsConfig(doc);
    REQUIRE(config.global_layer.read_only.has_value());
    CHECK(*config.global_layer.read_only == PermissionMode::AlwaysAllow);
    CHECK(*config.global_layer.file_write == PermissionMode::AlwaysDeny);
    CHECK(*config.global_layer.execute == PermissionMode::Ask);
    CHECK(*config.global_layer.out_of_workdir == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParsePermissionsConfig: top-level per_tool_overrides lands in global_layer", "[permissions_config]") {
    nlohmann::json doc = {{"per_tool_overrides", {{"bash", "ask"}}}};

    auto config = ParsePermissionsConfig(doc);
    REQUIRE(config.global_layer.per_tool_overrides.count("bash") == 1);
    CHECK(config.global_layer.per_tool_overrides.at("bash") == PermissionMode::Ask);
}

TEST_CASE("ParsePermissionsConfig: empty-string per-tool key throws", "[permissions_config]") {
    nlohmann::json doc = {{"per_tool_overrides", {{"", "allow"}}}};
    CHECK_THROWS_AS(ParsePermissionsConfig(doc), std::runtime_error);
}

TEST_CASE("ParsePermissionsConfig: all three default_run_mode values decode", "[permissions_config]") {
    CHECK(ParsePermissionsConfig({{"default_run_mode", "normal"}}).default_run_mode == PermissionRunMode::Normal);
    CHECK(ParsePermissionsConfig({{"default_run_mode", "bypass"}}).default_run_mode == PermissionRunMode::Bypass);
    CHECK(ParsePermissionsConfig({{"default_run_mode", "allow-only"}}).default_run_mode ==
          PermissionRunMode::AllowOnly);
}

TEST_CASE("ParsePermissionsConfig: mcp_default_mode overridable", "[permissions_config]") {
    CHECK(ParsePermissionsConfig({{"mcp_default_mode", "deny"}}).mcp_default_mode == PermissionMode::AlwaysDeny);
}

TEST_CASE("LoadPermissionsConfig: reads and parses a real file", "[permissions_config]") {
    minimal::testing::TempDir tempdir;
    const auto path = tempdir.path() / "permissions.json";
    { std::ofstream(path) << nlohmann::json{{"default_run_mode", "bypass"}}.dump(); }

    auto config = LoadPermissionsConfig(path);
    CHECK(config.default_run_mode == PermissionRunMode::Bypass);
}

TEST_CASE("LoadPermissionsConfig: missing file throws", "[permissions_config]") {
    CHECK_THROWS_AS(LoadPermissionsConfig("/nonexistent/permissions.json"), std::runtime_error);
}

TEST_CASE("ParsePermissionsConfig: advisor fields parse and default off", "[permissions_config][advisor]") {
    auto config = ParsePermissionsConfig(nlohmann::json::object());
    CHECK_FALSE(config.advisor.has_value());
    CHECK_FALSE(config.advisor_inherit);
    CHECK(config.advisor_timeout_ms == 30000);  // the default budget, on

    nlohmann::json doc = {{"advisor", "auto-approve"}, {"advisor_inherit", true},
                          {"advisor_timeout_ms", 5000}};
    config = ParsePermissionsConfig(doc);
    REQUIRE(config.advisor.has_value());
    CHECK(*config.advisor == "auto-approve");
    CHECK(config.advisor_inherit);
    CHECK(config.advisor_timeout_ms == 5000);
}

TEST_CASE("ParsePermissionsConfig: advisor_timeout_ms 0 disables the budget; negatives refused",
          "[permissions_config][advisor]") {
    auto config = ParsePermissionsConfig(nlohmann::json{{"advisor_timeout_ms", 0}});
    CHECK(config.advisor_timeout_ms == 0);

    bool threw = false;
    try {
        (void)ParsePermissionsConfig(nlohmann::json{{"advisor_timeout_ms", -1}});
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("advisor_timeout_ms") != std::string::npos);
    }
    CHECK(threw);  // silently disabling a safety timeout is never intended
}

TEST_CASE("ParsePermissionsConfig: advisor_inherit false does not enable the advisor",
          "[permissions_config][advisor]") {
    // advisor_inherit alone governs children; with no advisor named it is inert.
    nlohmann::json doc = {{"advisor_inherit", true}};
    auto config = ParsePermissionsConfig(doc);
    CHECK_FALSE(config.advisor.has_value());
    CHECK(config.advisor_inherit);
}
