#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

#include "minimal/config/providers_config.hpp"

using namespace minimal::config;

namespace {

// RAII env var override so tests don't leak state into each other -- a local
// copy, matching config_paths_test.cpp's convention of not sharing this.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};

nlohmann::json FullEntry() {
    return {{"name", "openai-main"},
            {"provider", "openai"},
            {"base_url", "https://api.openai.com/v1"},
            {"model_name", "gpt-4o-mini"},
            {"parameters", {{"temperature", 0.7}}},
            {"context_window_tokens", 128000}};
}

}  // namespace

TEST_CASE("ParseProvidersConfig: full entry decodes every field", "[providers_config]") {
    auto config = ParseProvidersConfig(nlohmann::json::array({FullEntry()}));

    REQUIRE(config.entries.size() == 1);
    const auto& entry = config.entries[0];
    CHECK(entry.name == "openai-main");
    CHECK(entry.provider == "openai");
    CHECK(entry.base_url == "https://api.openai.com/v1");
    CHECK(entry.model_name == "gpt-4o-mini");
    CHECK(entry.parameters == nlohmann::json{{"temperature", 0.7}});
    REQUIRE(entry.context_window_tokens.has_value());
    CHECK(*entry.context_window_tokens == 128000);
    CHECK(entry.api_key.empty());
}

TEST_CASE("ParseProvidersConfig: api_key_env resolves from the environment", "[providers_config]") {
    ScopedEnv env("MINIMAL_TEST_API_KEY", "sk-secret");
    auto entry = FullEntry();
    entry["api_key_env"] = "MINIMAL_TEST_API_KEY";

    auto config = ParseProvidersConfig(nlohmann::json::array({entry}));
    CHECK(config.entries[0].api_key == "sk-secret");
}

TEST_CASE("ParseProvidersConfig: unset api_key_env throws", "[providers_config]") {
    ScopedEnv env("MINIMAL_TEST_API_KEY_UNSET", nullptr);
    auto entry = FullEntry();
    entry["api_key_env"] = "MINIMAL_TEST_API_KEY_UNSET";

    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::array({entry})), std::runtime_error);
}

TEST_CASE("ParseProvidersConfig: both api_key and api_key_env set throws", "[providers_config]") {
    ScopedEnv env("MINIMAL_TEST_API_KEY", "sk-secret");
    auto entry = FullEntry();
    entry["api_key"] = "sk-literal";
    entry["api_key_env"] = "MINIMAL_TEST_API_KEY";

    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::array({entry})), std::runtime_error);
}

TEST_CASE("ParseProvidersConfig: neither api_key nor api_key_env set is not an error", "[providers_config]") {
    auto config = ParseProvidersConfig(nlohmann::json::array({FullEntry()}));
    CHECK(config.entries[0].api_key.empty());
}

TEST_CASE("ParseProvidersConfig: duplicate name throws", "[providers_config]") {
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::array({FullEntry(), FullEntry()})), std::runtime_error);
}

TEST_CASE("ParseProvidersConfig: missing required field throws", "[providers_config]") {
    for (const std::string& key : {"name", "provider", "base_url", "model_name"}) {
        auto entry = FullEntry();
        entry.erase(key);
        CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::array({entry})), std::runtime_error);
    }
}

TEST_CASE("ProvidersConfig::Find hit and miss", "[providers_config]") {
    auto config = ParseProvidersConfig(nlohmann::json::array({FullEntry()}));
    CHECK(config.Find("openai-main") != nullptr);
    CHECK(config.Find("nonexistent") == nullptr);
}

TEST_CASE("LoadProvidersConfig: reads and parses a real file", "[providers_config]") {
    const auto path = std::filesystem::temp_directory_path() / "minimal_providers_config_test.json";
    { std::ofstream(path) << nlohmann::json::array({FullEntry()}).dump(); }

    auto config = LoadProvidersConfig(path);
    REQUIRE(config.entries.size() == 1);
    CHECK(config.entries[0].name == "openai-main");

    std::filesystem::remove(path);
}

TEST_CASE("LoadProvidersConfig: missing file throws", "[providers_config]") {
    CHECK_THROWS_AS(LoadProvidersConfig("/nonexistent/providers.json"), std::runtime_error);
}
