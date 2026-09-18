#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/config/settings_config.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;

TEST_CASE("ParseSettingsConfig: empty doc yields every documented default", "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());

    CHECK(config.default_agent_preset.empty());
    CHECK(config.default_model.empty());
    CHECK(config.default_subagent_model.empty());
    CHECK(config.default_theme.empty());
    CHECK(config.max_concurrent_subagents == 4);
    CHECK(config.memory.auto_compact == true);
    CHECK(config.memory.context_compaction_trigger.kind == CompactionTriggerSettings::Kind::Never);
    CHECK(config.memory.compaction_keep_recent_tokens == 0);
    CHECK(config.memory.scrollback_page_size == 0);
    CHECK(config.memory.compaction_strategy == "structured");
    CHECK(config.tools.mcp.connect_timeout_ms == 10000);
    CHECK(config.tools.mcp.request_timeout_ms == 60000);
    CHECK(config.tools.mcp.max_list_pages == 100);
    CHECK(config.tools_view.stdout_buffer_lines == 100);
    CHECK(config.project_context.enabled == true);
    REQUIRE(config.project_context.files.size() == 2);
    CHECK(config.project_context.files[0] == "AGENTS.md");
    CHECK(config.project_context.files[1] == "CLAUDE.md");
    CHECK(config.project_context.max_bytes == 0);
    CHECK(config.project_context.search_ancestors == false);
    CHECK(config.session_db_path.empty());
    CHECK(config.logging.level == "info");
    CHECK(config.logging.max_size_mb == 10);
    CHECK(config.logging.max_files == 5);
}

TEST_CASE("ParseSettingsConfig: top-level fields overridable individually", "[settings_config]") {
    nlohmann::json doc = {{"default_agent_preset", "build"},
                           {"default_model", "openai-main"},
                           {"default_subagent_model", "openai-main/small"},
                           {"default_theme", "dark"},
                           {"max_concurrent_subagents", 8},
                           {"session_db_path", "/data/sessions.db"}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.default_agent_preset == "build");
    CHECK(config.default_model == "openai-main");
    CHECK(config.default_subagent_model == "openai-main/small");
    CHECK(config.default_theme == "dark");
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

TEST_CASE("ParseSettingsConfig: legacy context_compaction_trigger_tokens still parses",
          "[settings_config]") {
    // Pre-strategy config dirs must not silently stop compacting. The new object
    // wins when both are present.
    SECTION("legacy key alone") {
        const auto config = ParseSettingsConfig(
            {{"memory", {{"context_compaction_trigger_tokens", 100000}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::FixedTokens);
        CHECK(config.memory.context_compaction_trigger.tokens == 100000);
    }
    SECTION("legacy zero keeps its old meaning of never") {
        const auto config = ParseSettingsConfig(
            {{"memory", {{"context_compaction_trigger_tokens", 0}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::Never);
    }
    SECTION("new object wins over the legacy key") {
        const auto config =
            ParseSettingsConfig({{"memory", {{"context_compaction_trigger_tokens", 100000},
                                               {"context_compaction_trigger", {{"percent", 80}}}}}});
        CHECK(config.memory.context_compaction_trigger.kind ==
              CompactionTriggerSettings::Kind::PercentOfWindow);
        CHECK(config.memory.context_compaction_trigger.percent == 80);
    }
    SECTION("negative legacy value is fatal, not a silent never") {
        // A negative value used to fall out as Kind::Never -- the one
        // misconfiguration whose symptom is a context that quietly grows
        // until the backend starts rejecting requests. It must fail loudly
        // like the new-object path does.
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger_tokens", -1}}}}),
                        std::runtime_error);
    }
    SECTION("wrong-typed legacy value is fatal") {
        CHECK_THROWS_AS(ParseSettingsConfig(
                            {{"memory", {{"context_compaction_trigger_tokens", "big"}}}}),
                        std::runtime_error);
    }
}

TEST_CASE("ParseSettingsConfig: tools.raw carries the whole slice verbatim, including keys "
          "ParseTools does not recognize",
          "[settings_config]") {
    // Phase 3: main.cpp forwards this to every plugin as context.tools_settings
    // without going through the typed fields above -- a plugin's own top-level
    // key under "tools" (here "a_plugin_only_key") must survive intact.
    nlohmann::json doc = {{"tools", {{"bash", {{"timeout_ms", 5000}}},
                                     {"a_plugin_only_key", {{"anything", true}}}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.tools.raw == doc.at("tools"));
    CHECK(config.tools.raw.at("a_plugin_only_key") == nlohmann::json{{"anything", true}});
}

TEST_CASE("ParseSettingsConfig: tools.raw defaults to an empty object with no \"tools\" key",
          "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());
    CHECK(config.tools.raw == nlohmann::json::object());
}

TEST_CASE("ParseSettingsConfig: tools.mcp knobs overridable, max_list_pages must be positive",
          "[settings_config]") {
    nlohmann::json doc = {{"tools", {{"mcp", {{"connect_timeout_ms", 1000},
                                               {"request_timeout_ms", 2000},
                                               {"max_list_pages", 7}}}}}};
    auto config = ParseSettingsConfig(doc);
    CHECK(config.tools.mcp.connect_timeout_ms == 1000);
    CHECK(config.tools.mcp.request_timeout_ms == 2000);
    CHECK(config.tools.mcp.max_list_pages == 7);

    CHECK_THROWS_AS(ParseSettingsConfig(nlohmann::json{
                        {"tools", {{"mcp", {{"max_list_pages", 0}}}}}}), std::runtime_error);
    CHECK_THROWS_AS(ParseSettingsConfig(nlohmann::json{
                        {"tools", {{"mcp", {{"max_list_pages", -3}}}}}}), std::runtime_error);
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

TEST_CASE("ParseSettingsConfig: max_result_bytes defaults to 10 MiB, must not be negative",
         "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());
    CHECK(config.tools.max_result_bytes == 10 * 1024 * 1024);

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

TEST_CASE("ParseSettingsConfig: project_context overridable", "[settings_config]") {
    nlohmann::json doc = {{"project_context",
                            {{"enabled", false},
                             {"files", {"AGENTS.md", "MINIMAL.md"}},
                             {"max_bytes", 65536},
                             {"search_ancestors", true}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.project_context.enabled == false);
    REQUIRE(config.project_context.files.size() == 2);
    CHECK(config.project_context.files[0] == "AGENTS.md");
    CHECK(config.project_context.files[1] == "MINIMAL.md");
    CHECK(config.project_context.max_bytes == 65536);
    CHECK(config.project_context.search_ancestors == true);
}

TEST_CASE("ParseSettingsConfig: project_context rejects malformed shapes", "[settings_config]") {
    SECTION("a non-object block") {
        CHECK_THROWS_AS(ParseSettingsConfig({{"project_context", "on"}}), std::runtime_error);
    }
    SECTION("a non-array files") {
        CHECK_THROWS_AS(
            ParseSettingsConfig({{"project_context", {{"files", "AGENTS.md"}}}}),
            std::runtime_error);
    }
    SECTION("an empty or non-string entry") {
        CHECK_THROWS_AS(ParseSettingsConfig({{"project_context", {{"files", {""}}}}}),
                        std::runtime_error);
        CHECK_THROWS_AS(ParseSettingsConfig({{"project_context", {{"files", {7}}}}}),
                        std::runtime_error);
    }
    SECTION("a path, not a plain file name") {
        CHECK_THROWS_AS(
            ParseSettingsConfig({{"project_context", {{"files", {"docs/AGENTS.md"}}}}}),
            std::runtime_error);
        CHECK_THROWS_AS(
            ParseSettingsConfig({{"project_context", {{"files", {"../AGENTS.md"}}}}}),
            std::runtime_error);
    }
    SECTION("a negative max_bytes") {
        CHECK_THROWS_AS(ParseSettingsConfig({{"project_context", {{"max_bytes", -1}}}}),
                        std::runtime_error);
    }
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
