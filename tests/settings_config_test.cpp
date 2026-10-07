#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>

#include "minimal/config/settings_config.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;

TEST_CASE("ParseSettingsConfig: empty doc yields every documented default", "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());

    CHECK(config.default_agent_preset.empty());
    CHECK(config.default_model.empty());
    CHECK(config.default_subagent_model.empty());
    CHECK(config.max_concurrent_subagents == 4);
    CHECK(config.memory.auto_compact == true);
    CHECK(config.memory.context_compaction_trigger.kind == CompactionTriggerSettings::Kind::Never);
    CHECK(config.memory.compaction_keep_recent_tokens == 0);
    CHECK(config.memory.scrollback_page_size == 0);
    CHECK(config.memory.compaction_strategy == "summarize");
    CHECK(config.tools_view.stdout_buffer_lines == 100);
    CHECK(config.session_db_path.empty());
    CHECK(config.logging.level == "info");
    CHECK(config.logging.max_size_mb == 10);
    CHECK(config.logging.max_files == 5);
}

TEST_CASE("ParseSettingsConfig: top-level fields overridable individually", "[settings_config]") {
    nlohmann::json doc = {{"default_agent_preset", "build"},
                           {"default_model", "openai-main"},
                           {"default_subagent_model", "openai-main/small"},
                           {"max_concurrent_subagents", 8},
                           {"session_db_path", "/data/sessions.db"}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.default_agent_preset == "build");
    CHECK(config.default_model == "openai-main");
    CHECK(config.default_subagent_model == "openai-main/small");
    CHECK(config.max_concurrent_subagents == 8);
    CHECK(config.session_db_path == "/data/sessions.db");
}

TEST_CASE("ParseSettingsConfig: memory settings overridable", "[settings_config]") {
    nlohmann::json doc = {{"memory",
                             {{"auto_compact", false},
                              {"context_compaction_trigger", {{"tokens", 100000}}},
                              {"compaction_keep_recent_tokens", 20000},
                              {"scrollback_page_size", 50},
                              {"compaction_strategy", "drop-oldest"}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.memory.auto_compact == false);
    CHECK(config.memory.context_compaction_trigger.kind ==
          CompactionTriggerSettings::Kind::FixedTokens);
    CHECK(config.memory.context_compaction_trigger.tokens == 100000);
    CHECK(config.memory.compaction_keep_recent_tokens == 20000);
    CHECK(config.memory.scrollback_page_size == 50);
    CHECK(config.memory.compaction_strategy == "drop-oldest");
}

TEST_CASE("ParseSettingsConfig: compaction trigger is a one-of strategy choice",
          "[settings_config]") {
    SECTION("percent of the provider's context window") {
        const auto config = ParseSettingsConfig(
            {{"memory", {{"context_compaction_trigger", {{"percent", 80}}}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::PercentOfWindow);
        CHECK(config.memory.context_compaction_trigger.percent == 80);
    }
    SECTION("fixed token budget") {
        const auto config = ParseSettingsConfig(
            {{"memory", {{"context_compaction_trigger", {{"tokens", 120000}}}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::FixedTokens);
        CHECK(config.memory.context_compaction_trigger.tokens == 120000);
    }
    SECTION("buffer below the context window") {
        const auto config = ParseSettingsConfig(
            {{"memory", {{"context_compaction_trigger", {{"buffer_tokens", 16000}}}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::WindowBuffer);
        CHECK(config.memory.context_compaction_trigger.buffer_tokens == 16000);
    }
    SECTION("more than one strategy key is an error") {
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger",
                                           {{"percent", 80}, {"tokens", 120000}}}}}}),
                        std::runtime_error);
    }
    SECTION("an unknown key is an error, not a silent no-op") {
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger", {{"percnet", 80}}}}}}),
                        std::runtime_error);
    }
    SECTION("a non-object trigger is an error") {
        CHECK_THROWS_AS(ParseSettingsConfig({{"memory", {{"context_compaction_trigger", 80}}}}),
                        std::runtime_error);
    }
    SECTION("out-of-range values are errors") {
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger", {{"percent", 0}}}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger", {{"percent", 101}}}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger", {{"tokens", 0}}}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger",
                                           {{"buffer_tokens", -1}}}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger",
                                           {{"percent", "eighty"}}}}}}),
                        std::runtime_error);
    }
}

TEST_CASE("ParseSettingsConfig: context_compaction_trigger_tokens is an unknown field",
          "[settings_config]") {
    CHECK_THROWS_WITH(
        ParseSettingsConfig({{"memory", {{"context_compaction_trigger_tokens", 100000}}}}),
        Catch::Matchers::ContainsSubstring(
            "settings.json: memory.context_compaction_trigger_tokens: unknown field"));
}

TEST_CASE("ParseSettingsConfig: negative compaction_keep_recent_tokens is fatal",
          "[settings_config]") {
    CHECK_THROWS_AS(
        ParseSettingsConfig({{"memory", {{"compaction_keep_recent_tokens", -1}}}}),
        std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: the tools section is strict", "[settings_config]") {
    // Tool plugins take their knobs from plugins.json, not from here: a
    // tools.bash slice is an unknown field, not a silently ignored one.
    CHECK_THROWS_WITH(
        ParseSettingsConfig({{"tools", {{"bash", {{"timeout_ms", 5000}}}}}}),
        Catch::Matchers::ContainsSubstring("settings.json: tools.bash: unknown field"));
}

TEST_CASE("ParseSettingsConfig: unknown and wrong-shaped keys are named", "[settings_config]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(ParseSettingsConfig({{"defualt_model", "x"}}),
                      ContainsSubstring("settings.json: defualt_model: unknown field"));
    CHECK_THROWS_WITH(ParseSettingsConfig({{"memory", "oops"}}),
                      ContainsSubstring("settings.json: memory: must be an object"));
    CHECK_THROWS_WITH(ParseSettingsConfig({{"logging", {{"level", "verbose"}}}}),
                      ContainsSubstring("settings.json: logging.level: must be one of"));
    CHECK_NOTHROW(ParseSettingsConfig({{"// comment", "allowed"}}));
}

TEST_CASE("ParseSettingsConfig: max_parallel_tool_calls defaults to 4, must be positive",
          "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());
    CHECK(config.tools.max_parallel_tool_calls == 4);

    auto overridden = ParseSettingsConfig(
        nlohmann::json{{"tools", {{"max_parallel_tool_calls", 1}}}});
    CHECK(overridden.tools.max_parallel_tool_calls == 1);

    CHECK_THROWS_AS(ParseSettingsConfig(
                        nlohmann::json{{"tools", {{"max_parallel_tool_calls", 0}}}}),
                    std::runtime_error);
    CHECK_THROWS_AS(ParseSettingsConfig(
                        nlohmann::json{{"tools", {{"max_parallel_tool_calls", -2}}}}),
                    std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: max_result_bytes defaults to 256 KiB, must not be negative",
         "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());
    CHECK(config.tools.max_result_bytes == 256 * 1024);

    auto overridden =
        ParseSettingsConfig(nlohmann::json{{"tools", {{"max_result_bytes", 4096}}}});
    CHECK(overridden.tools.max_result_bytes == 4096);

    // 0 is a legal "disabled" value, unlike max_parallel_tool_calls.
    auto disabled =
        ParseSettingsConfig(nlohmann::json{{"tools", {{"max_result_bytes", 0}}}});
    CHECK(disabled.tools.max_result_bytes == 0);

    CHECK_THROWS_AS(ParseSettingsConfig(
                        nlohmann::json{{"tools", {{"max_result_bytes", -1}}}}),
                    std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: max_result_window_percent defaults to 10, 0..100",
         "[settings_config]") {
    CHECK(ParseSettingsConfig(nlohmann::json::object()).tools.max_result_window_percent == 10);
    CHECK(ParseSettingsConfig(nlohmann::json{{"tools", {{"max_result_window_percent", 0}}}})
              .tools.max_result_window_percent == 0);
    CHECK_THROWS_AS(ParseSettingsConfig(
                        nlohmann::json{{"tools", {{"max_result_window_percent", 101}}}}),
                    std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: max_display_bytes defaults to 128 KiB, must not be negative",
         "[settings_config]") {
    CHECK(ParseSettingsConfig(nlohmann::json::object()).tools.max_display_bytes == 128 * 1024);
    CHECK(ParseSettingsConfig(nlohmann::json{{"tools", {{"max_display_bytes", 0}}}})
              .tools.max_display_bytes == 0);
    CHECK_THROWS_AS(ParseSettingsConfig(
                        nlohmann::json{{"tools", {{"max_display_bytes", -1}}}}),
                    std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: tools_view and logging overridable", "[settings_config]") {
    nlohmann::json doc = {{"tools_view", {{"stdout_buffer_lines", 250}}},
                           {"logging", {{"level", "debug"}, {"path", "/var/log/minimal.log"},
                                        {"max_size_mb", 50}, {"max_files", 3}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.tools_view.stdout_buffer_lines == 250);
    CHECK(config.logging.level == "debug");
    CHECK(config.logging.path == "/var/log/minimal.log");
    CHECK(config.logging.max_size_mb == 50);
    CHECK(config.logging.max_files == 3);
}

TEST_CASE("ParseSettingsConfig: a wrong-typed value names its key", "[settings_config]") {
    const auto message = [](const nlohmann::json& doc) {
        try {
            ParseSettingsConfig(doc);
        } catch (const std::runtime_error& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    CHECK(message({{"default_model", 5}}).find("settings.json: default_model: must be a string") !=
          std::string::npos);
    CHECK(message({{"memory", {{"auto_compact", "yes"}}}}).find("memory.auto_compact") !=
          std::string::npos);
    CHECK(message({{"tools", {{"max_input_bytes", 1.5}}}}).find("tools.max_input_bytes") !=
          std::string::npos);
    // An integer that does not fit its field is an error, not a silent wrap.
    CHECK(message({{"tools", {{"max_parallel_tool_calls", 4294967296LL}}}}).find("must be at most 64") !=
          std::string::npos);
}

TEST_CASE("ParseSettingsConfig: server settings overridable, warm-up budget validated",
          "[settings_config]") {
    SECTION("defaults") {
        auto config = ParseSettingsConfig(nlohmann::json::object());
        CHECK(config.server.idle_shutdown_seconds == 5);
        CHECK(config.server.discovery_warmup_timeout_seconds == 15);
    }
    SECTION("overridable individually") {
        auto config = ParseSettingsConfig(
            {{"server", {{"idle_shutdown_seconds", 60}, {"discovery_warmup_timeout_seconds", 0}}}});
        CHECK(config.server.idle_shutdown_seconds == 60);
        CHECK(config.server.discovery_warmup_timeout_seconds == 0);
    }
    SECTION("negative values rejected") {
        CHECK_THROWS_AS(ParseSettingsConfig({{"server", {{"idle_shutdown_seconds", -1}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(
            ParseSettingsConfig({{"server", {{"discovery_warmup_timeout_seconds", -1}}}}),
            std::runtime_error);
    }
}

TEST_CASE("LoadSettingsConfig: reads and parses a real file", "[settings_config]") {
    minimal::testing::TempDir tempdir;
    const auto path = tempdir.path() / "settings.json";
    { std::ofstream(path) << nlohmann::json{{"default_model", "openai-main"}}.dump(); }

    auto config = LoadSettingsConfig(path);
    CHECK(config.default_model == "openai-main");
}

TEST_CASE("LoadSettingsConfig: missing file throws", "[settings_config]") {
    CHECK_THROWS_AS(LoadSettingsConfig("/nonexistent/settings.json"), std::runtime_error);
}

TEST_CASE("ParseSettingsConfig: compaction_replace_percent defaults to 80, accepts 1..100",
          "[settings_config]") {
    CHECK(ParseSettingsConfig(nlohmann::json::object()).memory.compaction_replace_percent == 80);
    CHECK(ParseSettingsConfig({{"memory", {{"compaction_replace_percent", 60}}}})
              .memory.compaction_replace_percent == 60);
    CHECK_THROWS(ParseSettingsConfig({{"memory", {{"compaction_replace_percent", 0}}}}));
    CHECK(ParseSettingsConfig({{"memory", {{"compaction_replace_percent", 100}}}})
              .memory.compaction_replace_percent == 100);
    CHECK_THROWS(ParseSettingsConfig({{"memory", {{"compaction_replace_percent", 101}}}}));
    CHECK_THROWS(ParseSettingsConfig({{"memory", {{"compaction_replace_percent", "80"}}}}));
}

TEST_CASE("ParseSettingsConfig: context_contributors", "[settings_config][context]") {
    const auto defaults = ParseSettingsConfig(nlohmann::json::object());
    CHECK(defaults.context_contributors.disabled.empty());
    CHECK(defaults.context_contributors.max_bytes_per_section == 16384);

    const auto parsed = ParseSettingsConfig(
        {{"context_contributors", {{"disabled", {"memory"}}, {"max_bytes_per_section", 0}}}});
    REQUIRE(parsed.context_contributors.disabled.size() == 1);
    CHECK(parsed.context_contributors.disabled[0] == "memory");
    CHECK(parsed.context_contributors.max_bytes_per_section == 0);

    CHECK_THROWS(ParseSettingsConfig({{"context_contributors", 1}}));
    CHECK_THROWS(ParseSettingsConfig({{"context_contributors", {{"disabled", "memory"}}}}));
    CHECK_THROWS(ParseSettingsConfig({{"context_contributors", {{"disabled", {""}}}}}));
    CHECK_THROWS(
        ParseSettingsConfig({{"context_contributors", {{"max_bytes_per_section", -1}}}}));
}
