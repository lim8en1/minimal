#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

#include "minimal/config/hooks_config.hpp"

using minimal::config::HookActionConfig;
using minimal::config::HooksConfig;
using minimal::config::LoadHooksConfig;
using minimal::config::ParseHooksConfig;

namespace {

std::string Write(const std::filesystem::path& dir, const std::string& name,
                  const std::string& contents) {
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream out(path);
    out << contents;
    return path.string();
}

}  // namespace

TEST_CASE("an empty document parses to the default (no hooks)", "[hooks_config]") {
    const auto config = ParseHooksConfig(nlohmann::json::object());
    CHECK(config.hooks.empty());
    CHECK(config.default_timeout_ms == 10000);
}

TEST_CASE("a full document parses", "[hooks_config]") {
    const nlohmann::json doc = {
        {"default_timeout_ms", 5000},
        {"hooks",
         {{{"on", "core.pre_tool_use"},
           {"filter", {{"tool_name", "bash"}}},
           {"command", "~/bin/guard.sh"},
           {"timeout_ms", 1234}}}}};

    const auto config = ParseHooksConfig(doc);
    REQUIRE(config.hooks.size() == 1);
    CHECK(config.default_timeout_ms == 5000);
    CHECK(config.hooks[0].on == "core.pre_tool_use");
    CHECK(config.hooks[0].command == "~/bin/guard.sh");
    REQUIRE(config.hooks[0].timeout_ms.has_value());
    CHECK(*config.hooks[0].timeout_ms == 1234);
    REQUIRE(config.hooks[0].filter.size() == 1);
    CHECK(config.hooks[0].filter[0] == std::make_pair(std::string("tool_name"),
                                                      std::string("bash")));
}

TEST_CASE("timeout_ms is optional and zero is a legal explicit opt-out", "[hooks_config]") {
    const nlohmann::json doc = {
        {"hooks", {{{"on", "core.stop"}, {"command", "x"}, {"timeout_ms", 0}}}}};
    const auto config = ParseHooksConfig(doc);
    REQUIRE(config.hooks.size() == 1);
    REQUIRE(config.hooks[0].timeout_ms.has_value());
    CHECK(*config.hooks[0].timeout_ms == 0);
}

TEST_CASE("the parse ladder rejects malformed documents by name", "[hooks_config]") {
    struct Case {
        std::string name;
        nlohmann::json doc;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {"a non-object top level", nlohmann::json::array(), "expected a JSON object"},
        {"a non-array hooks", nlohmann::json{{"hooks", 5}}, "'hooks' must be an array"},
        {"a non-object entry", nlohmann::json{{"hooks", {5}}}, "must be an object"},
        {"a missing on", nlohmann::json{{"hooks", {{{"command", "x"}}}}}, "'on'"},
        {"a non-string on", nlohmann::json{{"hooks", {{{"on", 5}, {"command", "x"}}}}},
         "non-empty string 'on'"},
        {"a missing command", nlohmann::json{{"hooks", {{{"on", "core.stop"}}}}}, "'command'"},
        {"a negative default timeout",
         nlohmann::json{{"default_timeout_ms", -1}, {"hooks", nlohmann::json::array()}},
         "must be >= 0"},
        {"a negative per-action timeout",
         nlohmann::json{{"hooks", {{{"on", "core.stop"}, {"command", "x"}, {"timeout_ms", -5}}}}},
         "must be >= 0"},
        {"a non-object filter",
         nlohmann::json{{"hooks", {{{"on", "core.stop"}, {"command", "x"}, {"filter", 5}}}}},
         "'filter' must be an object"},
        {"a non-string filter value",
         nlohmann::json{{"hooks",
                         {{{"on", "core.stop"},
                           {"command", "x"},
                           {"filter", {{"k", 5}}}}}}},
         "must be a string value"},
    };

    for (const auto& [name, doc, needle] : cases) {
        CAPTURE(name);
        REQUIRE_THROWS_WITH(ParseHooksConfig(doc), Catch::Matchers::ContainsSubstring(needle));
    }
}

TEST_CASE("a missing file is the normal no-hooks state", "[hooks_config]") {
    const auto config = LoadHooksConfig("/nonexistent/minimal/hooks.json");
    CHECK(config.hooks.empty());
}

TEST_CASE("a malformed FILE throws -- the sanity check reports it", "[hooks_config]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_config_test";
    Write(dir, "hooks.json", "{ not json ");
    // Phase 3: normalized onto the same wrapped std::runtime_error every other
    // loader throws for malformed JSON, instead of a raw nlohmann::json::parse_error.
    REQUIRE_THROWS_WITH(LoadHooksConfig(dir / "hooks.json"),
                        Catch::Matchers::ContainsSubstring("hooks.json: invalid JSON:"));
    std::filesystem::remove_all(dir);
}
