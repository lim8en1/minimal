// opencode-compat: opencode's provider shape and auth.json, read through the
// generic providers.json extension seam (config/provider_config_extensions.hpp).

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <nlohmann/json.hpp>

#include "minimal/config/providers_config.hpp"
#include "opencode_providers.hpp"

using namespace minimal;
using config::ModelRef;

namespace {

struct Translator final : config::ProviderEntryTranslator {
    void Translate(nlohmann::json& entry, const std::string& where) const override {
        entry = opencode_compat::TranslateOpencodeProvider(entry, where);
    }
};

config::ProvidersConfig Parse(const char* json) {
    config::ProviderConfigExtensions extensions;
    extensions.AddTranslator(std::make_shared<Translator>());
    return config::ParseProvidersConfig(nlohmann::json::parse(json), {}, &extensions);
}

}  // namespace

TEST_CASE("an opencode-shaped provider entry translates onto core's fields", "[opencode_compat]") {
    const auto config = Parse(R"json({
      "providers": {
        "local": {
          "npm": "@ai-sdk/openai-compatible",
          "name": "LM Studio (local)",
          "options": {
            "baseURL": "http://127.0.0.1:1234/v1",
            "apiKey": "lmstudio-key",
            "headers": {"X-Oven": "hot"}
          },
          "models": {
            "qwen": {"name": "Qwen (local)", "limit": {"context": 128000, "output": 65536}}
          }
        }
      }
    })json");
    const auto model = config.Resolve(ModelRef{"local", "qwen"});
    REQUIRE(model != nullptr);
    CHECK(model->backend == "openai");  // the one npm package that names a wire shape
    CHECK(model->provider.base_url == "http://127.0.0.1:1234/v1");
    CHECK(model->provider.api_key == "lmstudio-key");
    REQUIRE(model->context_window_tokens.has_value());
    CHECK(*model->context_window_tokens == 128000);
    REQUIRE(model->max_tokens.has_value());
    CHECK(*model->max_tokens == 65536);
    REQUIRE(model->provider.headers.size() == 1);
    CHECK(model->provider.headers[0].first == "X-Oven");
    CHECK(model->provider.headers[0].second == "hot");
}

TEST_CASE("without the plugin, an opencode-shaped entry is a named config error",
          "[opencode_compat]") {
    // Core knows only its own shape: no url (options.baseURL is not core's key).
    CHECK_THROWS(config::ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"npm": "@ai-sdk/openai-compatible",
                              "options": {"baseURL": "http://x/v1"}, "models": {"m": {}}}}
    })")));
}

TEST_CASE("an opencode-shaped model's options fold into parameters, native keys winning "
          "per-key",
          "[opencode_compat]") {
    // opencode's AI SDK spreads a model's `options` into the request body;
    // `parameters` is core's verbatim passthrough, so the fold makes an
    // opencode providers.json (e.g. OpenRouter's provider routing) work as-is.
    const auto config = Parse(R"json({
      "providers": {
        "openrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "models": {
            "glm": {"options": {"provider": {"only": ["z-ai/fp8"]}}},
            "kimi": {"options": {"provider": {"only": ["deepinfra/fp8"]}, "temperature": 0.1},
                     "parameters": {"temperature": 0.7}}
          }
        }
      }
    })json");
    const auto glm = config.Resolve(ModelRef{"openrouter", "glm"});
    REQUIRE(glm != nullptr);
    CHECK(glm->parameters == nlohmann::json{{"provider", {{"only", {"z-ai/fp8"}}}}});
    const auto kimi = config.Resolve(ModelRef{"openrouter", "kimi"});
    REQUIRE(kimi != nullptr);
    CHECK(kimi->parameters ==
          nlohmann::json{{"provider", {{"only", {"deepinfra/fp8"}}}}, {"temperature", 0.7}});
}

TEST_CASE("opencode's auth.json yields a provider's key from either spelling",
          "[opencode_compat]") {
    const auto doc = nlohmann::json::parse(
        R"({"a": {"type": "api", "key": "k1"}, "b": {"api_key": "k2"}, "c": {"key": ""}})");
    CHECK(opencode_compat::LookupOpencodeKey(doc, "a") == "k1");
    CHECK(opencode_compat::LookupOpencodeKey(doc, "b") == "k2");
    CHECK_FALSE(opencode_compat::LookupOpencodeKey(doc, "c").has_value());
    CHECK_FALSE(opencode_compat::LookupOpencodeKey(doc, "missing").has_value());
}
