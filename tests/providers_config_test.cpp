#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

#include "minimal/config/providers_config.hpp"
#include "temp_dir.hpp"

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

// One provider, two models with different context windows -- the shape the old
// flat schema could not express, so it is the default fixture here.
nlohmann::json TwoModelsOnOneProvider() {
    return nlohmann::json::parse(R"({
      "providers": {
        "local": {
          "backend": "openai",
          "url": "http://localhost:8000/v1",
          "max_tokens": 4096,
          "parameters": {"temperature": 0.3},
          "models": {
            "qwen4": {"context_window_tokens": 131072},
            "small": {"context_window_tokens": 32768, "max_tokens": 1024,
                      "parameters": {"temperature": 0.7}}
          }
        }
      }
    })");
}

const ResolvedModel& Require(const ProvidersConfig& config, const std::string& text) {
    const auto ref = ParseModelRef(text);
    REQUIRE(ref.has_value());
    const ResolvedModel* model = config.Resolve(*ref);
    REQUIRE(model != nullptr);
    return *model;
}

}  // namespace

// --- <provider>/<model> parsing ---------------------------------------------

TEST_CASE("ParseModelRef splits exactly one slash", "[providers]") {
    const auto ref = ParseModelRef("local/qwen4");
    REQUIRE(ref.has_value());
    CHECK(ref->provider == "local");
    CHECK(ref->model == "qwen4");
    CHECK(ref->str() == "local/qwen4");
}

TEST_CASE("ParseModelRef rejects anything it would have to guess about", "[providers]") {
    CHECK_FALSE(ParseModelRef("qwen4").has_value());       // no provider
    CHECK_FALSE(ParseModelRef("local/").has_value());      // no model
    CHECK_FALSE(ParseModelRef("/qwen4").has_value());      // no provider
    CHECK_FALSE(ParseModelRef("").has_value());
    CHECK_FALSE(ParseModelRef("/").has_value());
    // Taking the first slash here would silently produce a provider called "a".
    CHECK_FALSE(ParseModelRef("a/b/c").has_value());
}

// --- The case the old schema could not express -------------------------------

TEST_CASE("two models on one provider share a connection and differ in context",
          "[providers][invariant]") {
    // THE REASON FOR THE SCHEMA. Previously a provider entry *was* a model, so
    // one endpoint serving a 128k model and a 32k model needed two entries
    // duplicating the url -- and there was nowhere to put the second context
    // window, because context_window_tokens lived on the entry.
    const auto config = ParseProvidersConfig(TwoModelsOnOneProvider());

    const auto& big = Require(config, "local/qwen4");
    const auto& small = Require(config, "local/small");

    CHECK(big.provider.base_url == small.provider.base_url);
    CHECK(big.provider.model_name == "qwen4");
    CHECK(small.provider.model_name == "small");
    REQUIRE(big.context_window_tokens.has_value());
    REQUIRE(small.context_window_tokens.has_value());
    CHECK(*big.context_window_tokens == 131072);
    CHECK(*small.context_window_tokens == 32768);
}

TEST_CASE("a model inherits its provider's settings and may override them", "[providers]") {
    const auto config = ParseProvidersConfig(TwoModelsOnOneProvider());

    const auto& inherited = Require(config, "local/qwen4");
    REQUIRE(inherited.max_tokens.has_value());
    CHECK(*inherited.max_tokens == 4096);                        // from the provider
    CHECK(inherited.parameters.at("temperature") == 0.3);        // from the provider

    const auto& overridden = Require(config, "local/small");
    REQUIRE(overridden.max_tokens.has_value());
    CHECK(*overridden.max_tokens == 1024);                       // its own
    CHECK(overridden.parameters.at("temperature") == 0.7);       // its own
}

TEST_CASE("a model's parameters replace the provider's rather than merging", "[providers]") {
    // Shallow on purpose: what a model declares is exactly what it sends. A deep
    // merge would make a model's effective settings depend on a block elsewhere
    // in the file, which is the thing that is hard to reason about at 3am.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
        "parameters": {"temperature": 0.1, "top_p": 0.9},
        "models": {"m": {"parameters": {"temperature": 0.9}}}}}
    })"));

    const auto& model = Require(config, "p/m");
    CHECK(model.parameters.at("temperature") == 0.9);
    CHECK_FALSE(model.parameters.contains("top_p"));
}

TEST_CASE("backend and connection reach every model under the provider", "[providers]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "local": {"backend": "openai", "url": "http://localhost:8000/v1",
                  "models": {"a": {}, "b": {}}},
        "hosted": {"backend": "anthropic", "url": "https://api.example.com",
                   "models": {"big": {}}}
      }
    })"));

    CHECK(Require(config, "local/a").backend == "openai");
    CHECK(Require(config, "local/b").backend == "openai");
    CHECK(Require(config, "hosted/big").backend == "anthropic");
    CHECK(Require(config, "hosted/big").provider.base_url == "https://api.example.com");
}

// --- The migration boundary --------------------------------------------------

TEST_CASE("the old flat array is refused by name", "[providers][invariant]") {
    // A working file from a previous build. Failing as "expected an object"
    // would be true and useless; the person hitting this needs to know what
    // changed. Accepting it instead -- a dual-schema parse path -- fails here.
    const auto legacy = nlohmann::json::parse(R"([
      {"name":"local","provider":"openai","base_url":"http://x/v1","model_name":"m"}
    ])");

    try {
        ParseProvidersConfig(legacy);
        FAIL("expected the legacy array to be refused");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("old format") != std::string::npos);
        CHECK(message.find("<provider>/<model>") != std::string::npos);
        CHECK(message.find("providers.example.json") != std::string::npos);
    }
}

// --- Structural errors -------------------------------------------------------

TEST_CASE("a provider with no models is refused", "[providers]") {
    // Unaddressable: nothing could name it, so it is a mistake rather than an
    // empty-but-valid state.
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"openai","url":"http://x"}}})")),
                    std::runtime_error);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"openai","url":"http://x","models":{}}}})")),
                    std::runtime_error);
}

TEST_CASE("a missing backend or url is refused", "[providers]") {
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"url":"http://x","models":{"m":{}}}}})")),
                    std::runtime_error);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"openai","models":{"m":{}}}}})")),
                    std::runtime_error);
}

TEST_CASE("a slash inside a name is refused", "[providers]") {
    // Otherwise "a/b/c" could not be parsed back into a provider and a model.
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"o","url":"u","models":{"a/b":{}}}}})")),
                    std::runtime_error);
}

TEST_CASE("a fractional numeric field is refused by name, not truncated", "[providers][invariant]") {
    // is_number() is true for 4096.7 and get<int> truncates, so a typo silently
    // became a different, valid-looking limit. Fails with the fix reverted --
    // the revert makes this parse cleanly with max_tokens == 2048.
    const auto doc = nlohmann::json::parse(R"({
      "providers": {"p": {"backend":"openai","url":"http://x",
        "models":{"m":{"max_tokens": 2048.5}}}}
    })");
    try {
        ParseProvidersConfig(doc);
        FAIL("expected the fractional max_tokens to be refused");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("max_tokens") != std::string::npos);
        CHECK(message.find("integer") != std::string::npos);
        CHECK(message.find("p/m") != std::string::npos);
    }
}

TEST_CASE("a typo'd top-level key is refused where the mistake is", "[providers][invariant]") {
    // "provider" instead of "providers" used to parse as a valid EMPTY config,
    // and the failure surfaced much later as "providers.json is empty" pointing
    // at the preset. The message must name the key actually found. Fails with
    // the fix reverted: this used to return an empty config without throwing.
    const auto doc = nlohmann::json::parse(R"({"provider": {"p": {"backend":"openai","url":"http://x"}}})");
    try {
        ParseProvidersConfig(doc);
        FAIL("expected the typo'd key to be refused");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("\"providers\"") != std::string::npos);
        CHECK(message.find("provider") != std::string::npos);
    }
}

TEST_CASE("an empty config is valid and resolves nothing", "[providers]") {
    // What bundled/providers.json seeds. The server must start so the startup
    // sanity check can report every problem at once, rather than dying here.
    for (const char* text : {"{}", R"({"providers":{}})"}) {
        const auto config = ParseProvidersConfig(nlohmann::json::parse(text));
        CHECK(config.empty());
        CHECK(config.ModelRefs().empty());
        CHECK(config.Resolve(ModelRef{"local", "qwen4"}) == nullptr);
    }
}

// --- API keys ----------------------------------------------------------------

TEST_CASE("api_key_env resolves once, at load", "[providers]") {
    ScopedEnv env("MINIMAL_TEST_PROVIDER_KEY", "sk-from-env");
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend":"openai","url":"http://x","api_key_env":"MINIMAL_TEST_PROVIDER_KEY",
                          "models":{"m":{}}}}
    })"));
    CHECK(Require(config, "p/m").provider.api_key == "sk-from-env");
}

TEST_CASE("an unset api_key_env fails at startup, not at the first request", "[providers]") {
    ScopedEnv env("MINIMAL_TEST_PROVIDER_KEY", nullptr);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
                        "providers": {"p": {"backend":"openai","url":"http://x",
                          "api_key_env":"MINIMAL_TEST_PROVIDER_KEY","models":{"m":{}}}}})")),
                    std::runtime_error);
}

TEST_CASE("api_key and api_key_env together are refused", "[providers]") {
    ScopedEnv env("MINIMAL_TEST_PROVIDER_KEY", "sk-from-env");
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
                        "providers": {"p": {"backend":"openai","url":"http://x","api_key":"sk-literal",
                          "api_key_env":"MINIMAL_TEST_PROVIDER_KEY","models":{"m":{}}}}})")),
                    std::runtime_error);
}

TEST_CASE("no key at all is fine", "[providers]") {
    // A local llama.cpp needs none, and requiring a placeholder would be noise.
    const auto config = ParseProvidersConfig(TwoModelsOnOneProvider());
    CHECK(Require(config, "local/qwen4").provider.api_key.empty());
}

// --- Listing and file loading ------------------------------------------------

TEST_CASE("ModelRefs lists every model, sorted", "[providers]") {
    // What /providers shows and what a "no such model" error offers instead.
    // Sorted so two runs list identically.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "zeta":  {"backend":"o","url":"u","models":{"m2":{},"m1":{}}},
        "alpha": {"backend":"o","url":"u","models":{"only":{}}}
      }
    })"));
    const auto refs = config.ModelRefs();
    REQUIRE(refs.size() == 3);
    CHECK(refs[0] == "alpha/only");
    CHECK(refs[1] == "zeta/m1");
    CHECK(refs[2] == "zeta/m2");
}

TEST_CASE("LoadProvidersConfig reads a real file", "[providers]") {
    minimal::testing::TempDir dir;
    const auto path = dir.path() / "providers.json";
    std::ofstream(path) << TwoModelsOnOneProvider().dump();

    const auto config = LoadProvidersConfig(path);
    CHECK(Require(config, "local/qwen4").provider.model_name == "qwen4");
}

TEST_CASE("LoadProvidersConfig names the missing file", "[providers]") {
    minimal::testing::TempDir dir;
    CHECK_THROWS_AS(LoadProvidersConfig(dir.path() / "nope.json"), std::runtime_error);
}
