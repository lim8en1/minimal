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
    CHECK(config.plugin_slices.empty());
    CHECK_FALSE(config.global_layer.read_only.has_value());
    CHECK_FALSE(config.global_layer.file_write.has_value());
    CHECK_FALSE(config.global_layer.execute.has_value());
    CHECK_FALSE(config.global_layer.out_of_workdir.has_value());
    CHECK_FALSE(config.global_layer.network.has_value());
    CHECK(config.global_layer.per_tool_overrides.empty());
}

TEST_CASE("ParsePermissionsConfig: fallback.* lands in global_layer", "[permissions_config]") {
    nlohmann::json doc = {{"fallback",
                            {{"read_only", "allow"},
                             {"file_write", "deny"},
                             {"execute", "ask"},
                             {"out_of_workdir", "deny"},
                             {"network", "deny"}}}};

    auto config = ParsePermissionsConfig(doc);
    REQUIRE(config.global_layer.read_only.has_value());
    CHECK(*config.global_layer.read_only == PermissionMode::AlwaysAllow);
    CHECK(*config.global_layer.file_write == PermissionMode::AlwaysDeny);
    CHECK(*config.global_layer.execute == PermissionMode::Ask);
    CHECK(*config.global_layer.out_of_workdir == PermissionMode::AlwaysDeny);
    CHECK(*config.global_layer.network == PermissionMode::AlwaysDeny);
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

TEST_CASE("ParsePermissionsConfig: mcp_default_mode is an unknown key", "[permissions_config]") {
    const auto config = ParsePermissionsConfig({{"mcp_default_mode", "deny"}});
    CHECK(config.plugin_slices.empty());
    CHECK(config.unknown_keys == std::vector<std::string>{"mcp_default_mode"});
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

// --- pattern rules -----------------------------------------------------------------

TEST_CASE("ParsePermissionsConfig: pattern_overrides and external_directory_patterns land in "
          "global_layer",
          "[permissions_config][patterns]") {
    nlohmann::json doc = {
        {"pattern_overrides",
         {{"bash", {{"git *", "allow"}, {"git push --force *", "ask"}}},
          {"read", {{"**/.env", "deny"}}}}},
        {"external_directory_patterns", {{"/tmp/builds/**", "allow"}}}};

    auto config = ParsePermissionsConfig(doc);
    REQUIRE(config.global_layer.pattern_overrides.size() == 2);
    const auto& bash_rules = config.global_layer.pattern_overrides.at("bash");
    REQUIRE(bash_rules.size() == 2);
    CHECK(bash_rules[0].glob == "git *");
    CHECK(bash_rules[0].mode == PermissionMode::AlwaysAllow);
    CHECK(bash_rules[1].mode == PermissionMode::Ask);
    REQUIRE(config.global_layer.out_of_workdir_patterns.size() == 1);
    CHECK(config.global_layer.out_of_workdir_patterns[0].glob == "/tmp/builds/**");
}

TEST_CASE("ParsePermissionsConfig: malformed pattern shapes throw", "[permissions_config][patterns]") {
    CHECK_THROWS_AS(ParsePermissionsConfig(nlohmann::json{
                        {"pattern_overrides", {{"bash", "allow"}}}}),   // value must be a map
                    std::runtime_error);
    CHECK_THROWS_AS(ParsePermissionsConfig(nlohmann::json{
                        {"pattern_overrides", {{"", {{"x", "allow"}}}}}}),  // empty tool key
                    std::runtime_error);
    CHECK_THROWS_AS(ParsePermissionsConfig(nlohmann::json{
                        {"external_directory_patterns", {{"", "allow"}}}}),  // empty glob
                    std::runtime_error);
    CHECK_THROWS_AS(ParsePermissionsConfig(nlohmann::json{
                        {"pattern_overrides", {{"bash", {{"git *", "sometimes"}}}}}}),  // bad mode
                    std::runtime_error);
}

TEST_CASE("ParsePermissionsConfig: unknown keys are collected, not silently dropped",
          "[permissions_config]") {
    const auto config = ParsePermissionsConfig(nlohmann::json{
        {"fallback", {{"file_writes", "deny"}, {"execute", "ask"}}},
        {"pattern", {{"bash", {{"rm *", "deny"}}}}},
        {"_comment", "ignored by convention"},
        {"pattern_precedence", "specific"}});
    CHECK(config.unknown_keys == std::vector<std::string>{"pattern", "fallback.file_writes"});
    CHECK(config.global_layer.pattern_precedence == minimal::tools::PatternPrecedence::Specific);
    CHECK_THROWS(ParsePermissionsConfig(nlohmann::json{{"pattern_precedence", "loose"}}));
}
