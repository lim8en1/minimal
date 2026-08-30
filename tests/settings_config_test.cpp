#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/config/settings_config.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;

TEST_CASE("ParseSettingsConfig: empty doc yields every documented default", "[settings_config]") {
    auto config = ParseSettingsConfig(nlohmann::json::object());

    CHECK(config.default_agent_preset.empty());
    CHECK(config.default_provider.empty());
    CHECK(config.default_theme.empty());
    CHECK(config.max_concurrent_subagents == 4);
    CHECK(config.memory.auto_compact == true);
    CHECK(config.memory.context_compaction_trigger_tokens == 0);
    CHECK(config.memory.compaction_keep_recent_tokens == 0);
    CHECK(config.memory.scrollback_page_size == 0);
    CHECK(config.tools_view.stdout_buffer_lines == 100);
    CHECK(config.session_db_path.empty());
    CHECK(config.logging.level == "info");
    CHECK(config.logging.max_size_mb == 10);
    CHECK(config.logging.max_files == 5);
}

TEST_CASE("ParseSettingsConfig: top-level fields overridable individually", "[settings_config]") {
    nlohmann::json doc = {{"default_agent_preset", "build"},
                           {"default_provider", "openai-main"},
                           {"default_theme", "dark"},
                           {"max_concurrent_subagents", 8},
                           {"session_db_path", "/data/sessions.db"}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.default_agent_preset == "build");
    CHECK(config.default_provider == "openai-main");
    CHECK(config.default_theme == "dark");
    CHECK(config.max_concurrent_subagents == 8);
    CHECK(config.session_db_path == "/data/sessions.db");
}

TEST_CASE("ParseSettingsConfig: memory settings overridable", "[settings_config]") {
    nlohmann::json doc = {{"memory",
                            {{"auto_compact", false},
                             {"context_compaction_trigger_tokens", 100000},
                             {"compaction_keep_recent_tokens", 20000},
                             {"scrollback_page_size", 50}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.memory.auto_compact == false);
    CHECK(config.memory.context_compaction_trigger_tokens == 100000);
    CHECK(config.memory.compaction_keep_recent_tokens == 20000);
    CHECK(config.memory.scrollback_page_size == 50);
}

TEST_CASE("ParseSettingsConfig: tools settings pass through as raw JSON", "[settings_config]") {
    nlohmann::json doc = {{"tools", {{"bash", {{"timeout_ms", 5000}, {"max_output_bytes", 64}}}}}};

    auto config = ParseSettingsConfig(doc);
    CHECK(config.tools.bash == nlohmann::json{{"timeout_ms", 5000}, {"max_output_bytes", 64}});
    CHECK(config.tools.read == nlohmann::json::object());
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

TEST_CASE("LoadSettingsConfig: reads and parses a real file", "[settings_config]") {
    minimal::testing::TempDir tempdir;
    const auto path = tempdir.path() / "settings.json";
    { std::ofstream(path) << nlohmann::json{{"default_provider", "openai-main"}}.dump(); }

    auto config = LoadSettingsConfig(path);
    CHECK(config.default_provider == "openai-main");
}

TEST_CASE("LoadSettingsConfig: missing file throws", "[settings_config]") {
    CHECK_THROWS_AS(LoadSettingsConfig("/nonexistent/settings.json"), std::runtime_error);
}
