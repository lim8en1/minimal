// The shipped config files and the examples in docs/CONFIGURATION.md, run
// through the REAL parsers. Every parser is strict, so a key the docs name but
// the code does not read (or a renamed one) fails here instead of in a user's
// startup -- the drift that once documented tools-bash's timeouts as *_ms
// while the code read *_s.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/config/hooks_config.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"

#include "settings.hpp"  // plugins/server/decision-advisor

using namespace minimal::config;

namespace {

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    return nlohmann::json::parse(ReadText(path));
}

// The first ```json block after the `## <heading>` line of CONFIGURATION.md.
nlohmann::json DocExample(const std::string& heading) {
    const std::string doc = ReadText(std::filesystem::path(MINIMAL_DOCS_DIR) / "CONFIGURATION.md");
    const auto section = doc.find("\n## " + heading + "\n");
    REQUIRE(section != std::string::npos);
    const auto open = doc.find("```json\n", section);
    REQUIRE(open != std::string::npos);
    const auto body = open + 8;
    const auto close = doc.find("```", body);
    REQUIRE(close != std::string::npos);
    return nlohmann::json::parse(doc.substr(body, close - body));
}

}  // namespace

TEST_CASE("bundled config files parse through the real parsers", "[config_docs_drift]") {
    const std::filesystem::path bundled(MINIMAL_BUNDLED_DIR);
    CHECK_NOTHROW(ParseSettingsConfig(ReadJson(bundled / "settings.json")));
    CHECK_NOTHROW(ParsePermissionsConfig(ReadJson(bundled / "permissions.json")));
    CHECK_NOTHROW(ParseHooksConfig(ReadJson(bundled / "hooks.json")));
    CHECK_NOTHROW(ParseHooksConfig(ReadJson(bundled / "hooks.example.json")));
    CHECK_NOTHROW(ParseProvidersConfig(ReadJson(bundled / "providers.json")));
    // decision-advisor.json: every shipped profile must resolve on its own.
    const auto advisor = minimal::decision_advisor::LoadBundledSettings(
        bundled / minimal::decision_advisor::kBundledSettingsFile);
    for (const auto& [name, profile] : advisor.at("backends").items()) {
        if (name.rfind("//", 0) == 0) continue;
        INFO("decision-advisor backend " << name);
        CHECK_NOTHROW(minimal::decision_advisor::ParseSettings(advisor, {{"backend", name}}));
    }
}

TEST_CASE("providers.example.json parses, and every model it declares is an object",
          "[config_docs_drift]") {
    // Its env: references need not be set: an unset variable binds and only
    // fails per request.
    const auto config =
        ParseProvidersConfig(ReadJson(std::filesystem::path(MINIMAL_BUNDLED_DIR) /
                                      "providers.example.json"));
    CHECK(config.Resolve(ModelRef{"openrouter", "anthropic/claude-opus-5"}) != nullptr);
    // The provider-level recipe reached a model without its own variants.
    CHECK(config.Resolve(ModelRef{"openrouter", "anthropic/claude-opus-5@high"}) != nullptr);
    // ...and did not become a model named "variants".
    CHECK(config.Resolve(ModelRef{"openrouter", "variants"}) == nullptr);
}

TEST_CASE("the CONFIGURATION.md examples parse through the real parsers",
          "[config_docs_drift]") {
    CHECK_NOTHROW(ParseSettingsConfig(DocExample("settings.json")));
    CHECK_NOTHROW(ParseProvidersConfig(DocExample("providers.json")));
    CHECK_NOTHROW(ParsePermissionsConfig(DocExample("permissions.json")));
    CHECK_NOTHROW(ParseHooksConfig(DocExample("hooks.json")));
}

TEST_CASE("bundled and example agent presets parse through the real frontmatter parser",
          "[config_docs_drift]") {
    // The tool vocabulary of a build with every bundled plugin enabled, so a
    // `permission:` key naming a tool resolves as it would at startup.
    const minimal::agent::ToolVocabulary tools(std::vector<std::string>{
        "read", "write", "edit", "grep", "glob", "bash", "web_fetch", "web_search",
        "python_exec", "python_input", "python_interrupt", "python_output", "checklist",
        "ask_user", "message_session", "skill", "spawn_subagent", "memory_search",
        "memory_get", "memory_store", "memory_update", "memory_delete"});
    std::size_t parsed = 0;
    for (const char* dir : {MINIMAL_BUNDLED_DIR "/agents", MINIMAL_EXAMPLES_DIR "/agents"}) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() != ".md" || entry.path().filename() == "README.md") {
                continue;
            }
            CAPTURE(entry.path().string());
            CHECK_NOTHROW(minimal::agent::LoadAgentPresetFrontmatter(entry.path(), tools));
            ++parsed;
        }
    }
    CHECK(parsed >= 10);  // both directories were actually found
}
