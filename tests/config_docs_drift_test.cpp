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

#include "minimal/config/hooks_config.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"

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
