#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;
namespace testing = minimal::testing;
namespace llm = minimal::llm;

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

// A test-local fixture replicating provider-anthropic's AnthropicVariantParams
// (plugins/server/provider-anthropic/anthropic_variant_params.cpp) -- plugin
// code is not linked into this test binary, so the mapping is duplicated
// here rather than shared, the same way every other plugin-relocated
// behavior in this tree gets its own test-side fixture. Kept in exact sync
// with the plugin: any change to one should be mirrored in the other.
VariantParamsHook AnthropicVariantParamsHookFixture() {
    return [](const std::string& backend, const std::string& effort_word,
              std::optional<int> max_tokens) -> std::optional<std::pair<nlohmann::json, bool>> {
        if (backend != "anthropic") return std::nullopt;
        if (effort_word == "none") return std::pair{nlohmann::json::object(), false};
        std::optional<int> budget;
        if (effort_word == "low") budget = 4096;
        else if (effort_word == "medium") budget = 8192;
        else if (effort_word == "high") budget = 16384;
        if (!budget) {
            throw std::runtime_error(
                "providers.json: effort '" + effort_word +
                "' has no anthropic mapping (known: low, medium, high, xhigh, max, none)");
        }
        bool clamped = false;
        int value = *budget;
        if (max_tokens && value > *max_tokens - 1024) {
            value = *max_tokens - 1024;
            clamped = true;
        }
        return std::pair{
            nlohmann::json{{"thinking", {{"type", "enabled"}, {"budget_tokens", value}}}}, clamped};
    };
}

const ResolvedModel& Require(const ProvidersConfig& config, const std::string& text) {
    const auto ref = minimal::config::SplitModelRef(text);
    REQUIRE(ref.has_value());
    const auto model = config.Resolve(*ref);
    REQUIRE(model != nullptr);
    return *model;
}

}  // namespace

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

TEST_CASE("max_tokens_field parses onto the connection and validates its value",
          "[providers]") {
    // The output-cap key spelling for the OpenAI-shaped request body. Absent
    // means the historical "max_tokens" (an empty string on the connection);
    // the only accepted alternative is "max_completion_tokens" -- a typo is a
    // named parse error, not a pass-through no backend understands.
    auto doc = TwoModelsOnOneProvider();
    doc["providers"]["local"]["max_tokens_field"] = "max_completion_tokens";
    const auto config = ParseProvidersConfig(doc);
    const auto& model = Require(config, "local/qwen4");
    CHECK(model.provider.max_tokens_field == "max_completion_tokens");

    // The default: absent field -> empty on the connection -> the OpenAI
    // provider emits the historical key.
    const auto defaults = ParseProvidersConfig(TwoModelsOnOneProvider());
    CHECK(Require(defaults, "local/small").provider.max_tokens_field.empty());

    for (const nlohmann::json& bad :
         {nlohmann::json("max_completions_tokens"), nlohmann::json("tokens"),
          nlohmann::json(5)}) {
        nlohmann::json broken = TwoModelsOnOneProvider();
        broken["providers"]["local"]["max_tokens_field"] = bad;
        CHECK_THROWS_AS(ParseProvidersConfig(broken), std::runtime_error);
    }
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

TEST_CASE("slashes in names: the provider half is refused, the model half splits at the first one",
          "[providers]") {
    // A provider name containing '/' would make "a/b/c" unparseable back into
    // a provider and a model, so it is refused outright. A MODEL key containing
    // '/' is legal and splits at the FIRST '/' -- the same rule SplitModelRef
    // applies to interactive refs -- so OpenRouter ids are expressible as
    // static entries (see the OpenRouter-shaped case below).
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"a/b":{"backend":"o","url":"u","models":{"m":{}}}}})")),
                    std::runtime_error);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"o","url":"u","models":{"x/":{}}}}})")),
                    std::runtime_error);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(
                        R"({"providers":{"p":{"backend":"o","url":"u","models":{"/x":{}}}}})")),
                    std::runtime_error);

    // The key after the first '/' is the model whole -- including any further
    // slashes -- and is sent on the wire verbatim.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(
        R"({"providers":{"p":{"backend":"openai","url":"http://x",
            "models":{"vendor/model-slug":{}}}}})"));
    const auto model = config.Resolve(ModelRef{"p", "vendor/model-slug"});
    REQUIRE(model != nullptr);
    CHECK(model->ref.provider == "p");
    CHECK(model->ref.model == "vendor/model-slug");
    CHECK(model->provider.model_name == "vendor/model-slug");
    const auto refs = config.ModelRefs();
    REQUIRE(refs.size() == 1);
    CHECK(refs[0] == "p/vendor/model-slug");
}

TEST_CASE("a static OpenRouter-shaped entry resolves with its id verbatim and routes its "
          "parameters through",
          "[providers][compat]") {
    // The point of slash-bearing static model keys: OpenRouter ids are
    // vendor/model, the key is the wire "model" verbatim, and per-model
    // "parameters" carry OpenRouter's provider-routing body field. The
    // full ref is three segments and addressable from every surface that
    // names a model interactively (SplitModelRef).
    const ScopedEnv api_key("OPENROUTER_API_KEY", "test-key");
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "openrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "api_key": "env:OPENROUTER_API_KEY",
          "max_tokens": 8192,
          "models": {
            "anthropic/claude-opus-5": {
              "context_window_tokens": 200000,
              "parameters": {"provider": {"only": ["anthropic"]}}
            },
            "z-ai/glm-5.3": {}
          }
        }
      }
    })"));

    const auto opus = config.Resolve(ModelRef{"openrouter", "anthropic/claude-opus-5"});
    REQUIRE(opus != nullptr);
    CHECK(opus->provider.model_name == "anthropic/claude-opus-5");
    CHECK(opus->context_window_tokens == 200000);
    // The model with no own limits inherits the provider-level default.
    CHECK(opus->max_tokens == 8192);
    CHECK(opus->parameters == nlohmann::json{{"provider", {{"only", {"anthropic"}}}}});

    const auto glm = config.Resolve(ModelRef{"openrouter", "z-ai/glm-5.3"});
    REQUIRE(glm != nullptr);
    CHECK(glm->provider.model_name == "z-ai/glm-5.3");
    CHECK(glm->parameters.empty());

    const auto refs = config.ModelRefs();
    REQUIRE(refs.size() == 2);
    CHECK(refs[0] == "openrouter/anthropic/claude-opus-5");
    CHECK(refs[1] == "openrouter/z-ai/glm-5.3");
}

TEST_CASE("whitelist filters a slash-bearing static model id by its full key",
          "[providers][compat]") {
    // The ModelRefs() picker filter splits each catalog key at the FIRST '/',
    // so provider "openrouter" must keep matching a key like
    // "openrouter/anthropic/claude-opus-5" whose model half itself contains
    // '/' -- the filter and the parser must agree on the split rule.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "openrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "whitelist": ["anthropic/claude-opus-5"],
          "models": {
            "anthropic/claude-opus-5": {},
            "z-ai/glm-5.3": {}
          }
        }
      }
    })"));

    const auto refs = config.ModelRefs();
    REQUIRE(refs.size() == 1);
    CHECK(refs[0] == "openrouter/anthropic/claude-opus-5");
    // Filtering is a listing concern; resolution is untouched.
    CHECK(config.Resolve(ModelRef{"openrouter", "z-ai/glm-5.3"}) != nullptr);
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

TEST_CASE("an env: api_key resolves at load", "[providers]") {
    ScopedEnv env("MINIMAL_TEST_PROVIDER_KEY", "sk-from-env");
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend":"openai","url":"http://x","api_key":"env:MINIMAL_TEST_PROVIDER_KEY",
                          "models":{"m":{}}}}
    })"));
    const auto key = Require(config, "p/m").provider.api_key.Get();
    CHECK(key.value == "sk-from-env");
    CHECK(key.source == "env MINIMAL_TEST_PROVIDER_KEY");
}

TEST_CASE("an unset env: api_key fails at startup, not at the first request", "[providers]") {
    ScopedEnv env("MINIMAL_TEST_PROVIDER_KEY", nullptr);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
                        "providers": {"p": {"backend":"openai","url":"http://x",
                          "api_key":"env:MINIMAL_TEST_PROVIDER_KEY","models":{"m":{}}}}})")),
                    std::runtime_error);
}

TEST_CASE("a malformed secret: api_key fails at startup", "[providers]") {
    for (const char* ref : {"secret:", "secret::name", "secret:plugin:"}) {
        CAPTURE(ref);
        nlohmann::json doc = nlohmann::json::parse(R"({
          "providers": {"p": {"backend":"openai","url":"http://x","models":{"m":{}}}}})");
        doc["providers"]["p"]["api_key"] = ref;
        CHECK_THROWS_AS(ParseProvidersConfig(doc), std::runtime_error);
    }
}

TEST_CASE("a literal api_key is sent as-is and described without its value", "[providers]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend":"openai","url":"http://x","api_key":"sk-literal",
                          "models":{"m":{}}}}
    })"));
    const auto& key = Require(config, "p/m").provider.api_key;
    CHECK(key.Get().value == "sk-literal");
    CHECK(key.description() == "inline api_key");
}

TEST_CASE("no key at all is fine", "[providers]") {
    // A local llama.cpp needs none, and requiring a placeholder would be noise.
    const auto config = ParseProvidersConfig(TwoModelsOnOneProvider());
    CHECK_FALSE(Require(config, "local/qwen4").provider.api_key.configured());
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

// --- Discovery (track N, docs/EXTENSIBILITY.md §2.6) -------------------------

namespace {

// A hook with a call counter: the lazy/negative-cache properties are about HOW
// OFTEN the backend is asked, so the tests assert on the count, not just the
// result. Captured by reference everywhere; the hook also records the
// connection it was called with, so a test can assert it reached the catalog.
struct CountingHook {
    int calls = 0;
    bool ok = true;
    std::string error;
    std::vector<llm::DiscoveredModel> models;
    std::string last_kind;
    std::string last_url;

    DiscoveryOutcome AsHook(const std::string& kind, const llm::ProviderConfig& connection) {
        ++calls;
        last_kind = kind;
        last_url = connection.base_url;
        DiscoveryOutcome outcome;
        outcome.catalog_present = true;
        outcome.ok = ok;
        outcome.error = error;
        outcome.models = models;
        return outcome;
    }
};

ProvidersConfig DiscoverConfig() {
    // One discover-only entry (no static models) and one discover entry with a
    // static model whose context window is omitted -- the enrichment case. No
    // provider-level context_window_tokens here: an inherited default would
    // make "omitted" false and the enrichment path unreachable. The fallback
    // test below builds its own config for exactly that case.
    return ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "local": {"backend": "openai", "url": "http://localhost:8000/v1", "discover": true,
                  "max_tokens": 4096},
        "mixed": {"backend": "openai", "url": "http://mixed:8000/v1", "discover": true,
                  "models": {"static1": {}, "windowed": {"context_window_tokens": 16384}}}
      }
    })"));
}

}  // namespace

TEST_CASE("a discover-entry with no models parses", "[providers][discover]") {
    // The whole point: the backend names the models, not the file.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true}}
    })"));
    CHECK(config.Resolve(ModelRef{"p", "anything"}) == nullptr);  // cold, no hook: miss
    REQUIRE(config.provider_entries().count("p") == 1);
    CHECK(config.provider_entries().at("p").discover);
    CHECK(config.provider_entries().at("p").connection.base_url == "http://x/v1");
}

TEST_CASE("a non-discover provider with no models is still refused", "[providers][discover]") {
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": false}}
    })")), std::runtime_error);
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": "yes"}}
    })")), std::runtime_error);
}

TEST_CASE("a resolve before the discovery hook existed is not cached past the hook",
          "[providers][discover]") {
    // The startup sanity check resolves against the catalog before plugins'
    // catalogs are wired in; that miss must not stick once the hook arrives.
    CountingHook hook;
    hook.models = {{"big", 131072, nlohmann::json::object(), 32768}};
    auto config = DiscoverConfig();
    CHECK(config.Resolve(ModelRef{"local", "big"}) == nullptr);  // no hook yet

    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    const auto big = config.Resolve(ModelRef{"local", "big"});
    REQUIRE(big != nullptr);
    CHECK(hook.calls == 1);
}

TEST_CASE("a discover-only model resolves lazily, once", "[providers][discover]") {
    CountingHook hook;
    hook.models = {{"big", 131072, nlohmann::json::object(), 32768},
                   {"small", std::nullopt, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    // Cold: the miss triggers the fetch, the model resolves with the provider's
    // defaults and the discovered window.
    const auto big = config.Resolve(ModelRef{"local", "big"});
    REQUIRE(big != nullptr);
    CHECK(hook.calls == 1);
    CHECK(hook.last_kind == "openai");
    CHECK(hook.last_url == "http://localhost:8000/v1");
    CHECK(big->ref.str() == "local/big");
    CHECK(big->backend == "openai");
    CHECK(big->provider.model_name == "big");
    REQUIRE(big->max_tokens.has_value());
    CHECK(*big->max_tokens == 32768);  // the backend's cap beats the provider default
    REQUIRE(big->context_window_tokens.has_value());
    CHECK(*big->context_window_tokens == 131072);

    // A model the backend caps nothing: the provider-level default stands.
    const auto small = config.Resolve(ModelRef{"local", "small"});
    REQUIRE(small != nullptr);
    REQUIRE(small->max_tokens.has_value());
    CHECK(*small->max_tokens == 4096);

    // Warm: a second resolve of the same ref -- and a miss on a model the
    // catalog does not serve -- hit the cache, never the network.
    CHECK(config.Resolve(ModelRef{"local", "big"}) == big);
    CHECK(config.Resolve(ModelRef{"local", "absent"}) == nullptr);
    CHECK(hook.calls == 1);
}

TEST_CASE("a failed discovery is negative-cached and reported", "[providers][discover]") {
    CountingHook hook;
    hook.ok = false;
    hook.error = "connection refused";
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    CHECK(config.Resolve(ModelRef{"local", "big"}) == nullptr);
    CHECK(config.Resolve(ModelRef{"local", "big"}) == nullptr);
    CHECK(hook.calls == 1);  // not re-attempted per resolve

    const auto status = config.discovery_status("local");
    CHECK(status.attempted);
    CHECK_FALSE(status.ok);
    CHECK(status.error.find("connection refused") != std::string::npos);

    // A non-discover provider reports untouched, and a discover-entry never
    // resolved against is untouched too.
    CHECK_FALSE(config.discovery_status("mixed").attempted);
}

TEST_CASE("ModelRefs unions static and cached-discovered entries", "[providers][discover]") {
    CountingHook hook;
    hook.models = {{"zdisc", 4096, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    // Cold cache contributes none -- a listing is not a trigger to fetch.
    const auto cold = config.ModelRefs();
    REQUIRE(cold.size() == 2);
    CHECK(cold[0] == "mixed/static1");
    CHECK(cold[1] == "mixed/windowed");

    (void)config.Resolve(ModelRef{"local", "zdisc"});
    const auto warm = config.ModelRefs();
    REQUIRE(warm.size() == 3);
    CHECK(warm[0] == "local/zdisc");  // sorted, not appended
    CHECK(warm[1] == "mixed/static1");
    CHECK(warm[2] == "mixed/windowed");
}
TEST_CASE("ModelRefs lists a discovered model's variant siblings once the cache is warm",
          "[providers][variants][discover][regression]") {
    // THE BUG: discovery materialized the base model into the listing but not
    // its autodetected variant siblings -- /think offered three variants
    // (VariantsOf read the discovery cache) while the /model picker and
    // --model showed the bare model only, and <provider>/<model>@<name> was
    // the only way to address a sibling the config fully served. The listing
    // must be the same union the resolver serves: every materialized
    // discovery-cache variant ref, not just the materialized bases.
    CountingHook hook;
    hook.models = {
        {"m", 131072,
         nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["max", "high", "low"]}})"),
         std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    // Cold cache: bases only (no fetch trigger, the standing rule).
    const auto cold = config.ModelRefs();
    CHECK(std::find(cold.begin(), cold.end(), "local/m@max") == cold.end());

    REQUIRE(config.Resolve(ModelRef{"local", "m"}) != nullptr);  // warms the cache
    const auto warm = config.ModelRefs();
    CHECK(std::find(warm.begin(), warm.end(), "local/m") != warm.end());
    CHECK(std::find(warm.begin(), warm.end(), "local/m@max") != warm.end());
    CHECK(std::find(warm.begin(), warm.end(), "local/m@high") != warm.end());
    CHECK(std::find(warm.begin(), warm.end(), "local/m@low") != warm.end());
    // Models without an advertised ladder still list no siblings.
    CHECK(std::find_if(warm.begin(), warm.end(), [](const std::string& ref) {
              return ref.find("mixed/static1@") == 0 || ref.find("mixed/windowed@") == 0;
          }) == warm.end());
}

TEST_CASE("ModelRefs dedupes variant siblings against static siblings",
          "[providers][variants][discover]") {
    // A static twin with declared variants already put its siblings in the
    // catalog; the discovery cache records the recipe too. The union must not
    // list either twice -- and the static twin's own ladder wins (the same
    // explicit-beats-autodetection rule VariantsOf applies), so a recipe the
    // static entry opted out of adds nothing.
    CountingHook hook;
    hook.models = {
        {"m", 131072,
         nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["max", "high", "low"]}})"),
         std::nullopt}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "url": "http://localhost:8000/v1",
                              "discover": true, "models": {"m": {"variants": ["low"]}}}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    REQUIRE(config.Resolve(ModelRef{"local", "m"}) != nullptr);
    const auto refs = config.ModelRefs();
    CHECK(std::count(refs.begin(), refs.end(), "local/m") == 1);
    CHECK(std::count(refs.begin(), refs.end(), "local/m@low") == 1);
    // The static twin declared [low] only: @max/@high stay undiscoverable...
    CHECK(std::find(refs.begin(), refs.end(), "local/m@max") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "local/m@high") == refs.end());
    // ...and ResolveVariant agrees (a static sibling simply IS a catalog
    // entry), so the listing and the resolver cannot disagree here either.
    CHECK(config.Resolve(ModelRef{"local", "m@low"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"local", "m@max"}) == nullptr);

    // An explicit `variants: []` opt-out lists no siblings at all.
    CountingHook hook2 = hook;
    auto config2 = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "url": "http://localhost:8000/v1",
                              "discover": true, "models": {"m": {"variants": []}}}}
    })"));
    config2.SetDiscoveryHook([&hook2](const std::string& kind, const llm::ProviderConfig& c) {
        return hook2.AsHook(kind, c);
    });
    REQUIRE(config2.Resolve(ModelRef{"local", "m"}) != nullptr);
    const auto refs2 = config2.ModelRefs();
    CHECK(std::find(refs2.begin(), refs2.end(), "local/m") != refs2.end());
    CHECK(std::find_if(refs2.begin(), refs2.end(), [](const std::string& ref) {
              return ref.rfind("local/m@", 0) == 0;
          }) == refs2.end());
}

TEST_CASE("ModelRefs filters a discovered variant by its base id",
          "[providers][variants][discover]") {
    // The picker filter matches the BASE id for static variants ("hiding a
    // model must hide its variants"); the discovered union extends that rule
    // to discovery-cache siblings, which never reach the catalog keys the old
    // filter walked.
    CountingHook hook;
    hook.models = {
        {"m", 131072,
         nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["max", "high", "low"]}})"),
         std::nullopt}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "url": "http://localhost:8000/v1",
                              "discover": true, "blacklist": ["m"]}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    REQUIRE(config.Resolve(ModelRef{"local", "m"}) != nullptr);
    const auto refs = config.ModelRefs();
    CHECK(std::find(refs.begin(), refs.end(), "local/m") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "local/m@max") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "local/m@low") == refs.end());
}

TEST_CASE("a static model with an omitted window is enriched from discovery",
          "[providers][discover][invariant]") {
    // THE PAYOFF: compaction tracks the model actually served instead of a
    // number someone typed (or omitted, silently disabling compaction).
    // Resolving this window from the static entry alone fails this test.
    CountingHook hook;
    hook.models = {{"static1", 999999, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    const auto enriched = config.Resolve(ModelRef{"mixed", "static1"});
    REQUIRE(enriched != nullptr);
    REQUIRE(enriched->context_window_tokens.has_value());
    CHECK(*enriched->context_window_tokens == 999999);  // from discovery, not the 8192 default

    // A static model with its OWN window is untouched by discovery.
    const auto windowed = config.Resolve(ModelRef{"mixed", "windowed"});
    REQUIRE(windowed != nullptr);
    CHECK(*windowed->context_window_tokens == 16384);
    // ... and the enrichment did not overwrite the parsed entry for it.
    CHECK(config.Resolve(ModelRef{"mixed", "static1"}) == enriched);
    CHECK(hook.calls == 1);
}

TEST_CASE("a discovered window falls back to the provider-level default",
          "[providers][discover]") {
    // A backend that reports no window for a model: the provider-level
    // context_window_tokens is the inheritance base, exactly as it is for a
    // static model.
    CountingHook hook;
    hook.models = {{"small", std::nullopt, nlohmann::json::object(), std::nullopt}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true,
                          "context_window_tokens": 8192}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    const auto model = config.Resolve(ModelRef{"p", "small"});
    REQUIRE(model != nullptr);
    REQUIRE(model->context_window_tokens.has_value());
    CHECK(*model->context_window_tokens == 8192);
}

TEST_CASE("a static model's output cap is gap-filled from discovery, its own value standing",
          "[providers][discover][invariant]") {
    // The max_tokens twin of the window-enrichment case: a static entry that
    // omits the cap gets the backend's; one that declares its own keeps it --
    // and a window is no longer the only field that can trigger enrichment,
    // so this also pins that the two fills are independent.
    CountingHook hook;
    hook.models = {{"windowed", std::nullopt, nlohmann::json::object(), 4096},
                   {"capped", 999999, nlohmann::json::object(), 2048},
                   {"plain", std::nullopt, nlohmann::json::object(), std::nullopt}};
    // No provider-level defaults: a static model's INHERITED default counts as
    // its own value (the same rule the window enrichment follows), so a
    // default here would close the gap the backend cap is meant to fill.
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true,
                          "models": {
                            "windowed": {"context_window_tokens": 16384},
                            "capped": {"max_tokens": 999},
                            "plain": {}
                          }}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    // Own window stands; the missing cap comes from the backend.
    const auto windowed = config.Resolve(ModelRef{"p", "windowed"});
    REQUIRE(windowed != nullptr);
    CHECK(*windowed->context_window_tokens == 16384);
    REQUIRE(windowed->max_tokens.has_value());
    CHECK(*windowed->max_tokens == 4096);

    // Own cap stands; the missing window comes from the backend.
    const auto capped = config.Resolve(ModelRef{"p", "capped"});
    REQUIRE(capped != nullptr);
    CHECK(*capped->context_window_tokens == 999999);
    REQUIRE(capped->max_tokens.has_value());
    CHECK(*capped->max_tokens == 999);

    // Neither own value, and the backend reports neither: nothing is
    // fabricated -- the resolution just has no limits, as configured.
    const auto plain = config.Resolve(ModelRef{"p", "plain"});
    REQUIRE(plain != nullptr);
    CHECK_FALSE(plain->context_window_tokens.has_value());
    CHECK_FALSE(plain->max_tokens.has_value());
}

TEST_CASE("a failed discovery degrades a static hit to its parsed self",
          "[providers][discover]") {
    // A dead backend must not take a hand-written model down with it. This
    // matters more now that a static model missing only the output cap reaches
    // discovery (a windowed one used to resolve without consulting the catalog
    // at all) -- but the degradation is the rule for every static hit: the
    // negative-cached failure is reported via discovery_status, not by
    // unresolving models the config itself declares.
    CountingHook hook;
    hook.ok = false;
    hook.error = "connection refused";
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true,
                          "max_tokens": 4096,
                          "models": {"windowed": {"context_window_tokens": 16384},
                                     "bare": {}}}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    const auto windowed = config.Resolve(ModelRef{"p", "windowed"});
    REQUIRE(windowed != nullptr);
    CHECK(*windowed->context_window_tokens == 16384);
    CHECK(*windowed->max_tokens == 4096);

    const auto bare = config.Resolve(ModelRef{"p", "bare"});
    REQUIRE(bare != nullptr);
    CHECK(*bare->max_tokens == 4096);
    CHECK_FALSE(bare->context_window_tokens.has_value());

    // The failure is still reported, and still only fetched once.
    CHECK(hook.calls == 1);
    const auto status = config.discovery_status("p");
    CHECK(status.attempted);
    CHECK_FALSE(status.ok);
}

TEST_CASE("RefreshDiscovery evicts and refetches, reporting per provider",
          "[providers][discover]") {
    // The mid-process drift case: the backend restarts serving a different
    // model set while the server stays up. Refresh makes the new set visible
    // without a server restart.
    CountingHook hook;
    hook.models = {{"old", 4096, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    REQUIRE(config.Resolve(ModelRef{"local", "old"}) != nullptr);

    hook.models = {{"new", 8192, nlohmann::json::object(), std::nullopt}};
    const auto outcomes = config.RefreshDiscovery();
    REQUIRE(outcomes.size() == 2);  // both discover-entries
    CHECK(outcomes[0].provider == "local");
    CHECK(outcomes[0].ok);
    CHECK(outcomes[0].model_count == 1);

    // The stale model is gone from both listing and resolution; the new one is
    // served.
    CHECK(config.Resolve(ModelRef{"local", "old"}) == nullptr);
    const auto fresh = config.Resolve(ModelRef{"local", "new"});
    REQUIRE(fresh != nullptr);
    CHECK(*fresh->context_window_tokens == 8192);
}

TEST_CASE("RefreshDiscovery reports a dead catalog instead of throwing",
          "[providers][discover]") {
    CountingHook hook;
    hook.ok = false;
    hook.error = "timed out";
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    const auto outcomes = config.RefreshDiscovery();
    REQUIRE(outcomes.size() == 2);
    CHECK_FALSE(outcomes[0].ok);
    CHECK(outcomes[0].error.find("timed out") != std::string::npos);
}

TEST_CASE("a failed discovery is retried after its backoff, not on every resolve",
          "[providers][discover]") {
    // The backend started AFTER the server's first look: the negative cache
    // must expire on its own instead of waiting for a manual refresh.
    CountingHook hook;
    hook.ok = false;
    hook.error = "connection refused";
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    config.SetDiscoveryRetryBackoff(std::chrono::milliseconds(50));

    CHECK(config.Resolve(ModelRef{"local", "up-later"}) == nullptr);
    const int after_first = hook.calls;
    CHECK(config.Resolve(ModelRef{"local", "up-later"}) == nullptr);
    CHECK(hook.calls == after_first);  // inside the backoff: no refetch

    hook.ok = true;
    hook.models = {{"up-later", 8192, nlohmann::json::object(), std::nullopt}};
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto resolved = config.Resolve(ModelRef{"local", "up-later"});
    REQUIRE(resolved != nullptr);
    CHECK(*resolved->context_window_tokens == 8192);
    CHECK(hook.calls > after_first);
    CHECK(config.discovery_status("local").ok);
}

TEST_CASE("a failed refresh keeps the last good discovery", "[providers][discover]") {
    CountingHook hook;
    hook.models = {{"old", 4096, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    REQUIRE(config.Resolve(ModelRef{"local", "old"}) != nullptr);

    hook.ok = false;
    hook.error = "connection refused";
    const auto outcomes = config.RefreshDiscovery();
    REQUIRE(outcomes.size() == 2);
    CHECK_FALSE(outcomes[0].ok);
    CHECK(outcomes[0].error.find("connection refused") != std::string::npos);
    CHECK(outcomes[0].model_count == 1);

    // The model that resolved before the outage still resolves.
    CHECK(config.Resolve(ModelRef{"local", "old"}) != nullptr);
    CHECK(config.discovery_status("local").ok);
}

TEST_CASE("without a hook, discovery reports 'no discovery hook set'",
          "[providers][discover]") {
    // Tests and unhosted configs: a distinguishable state, not a silent
    // success-with-zero-models.
    auto config = DiscoverConfig();
    const auto model = config.Resolve(ModelRef{"local", "big"});
    CHECK(model == nullptr);
    const auto status = config.discovery_status("local");
    CHECK(status.attempted);
    CHECK_FALSE(status.ok);
    CHECK(status.error.find("no discovery hook") != std::string::npos);
}

TEST_CASE("concurrent Resolve is safe and fetches once", "[providers][discover]") {
    // The command pool runs beside session workers, so Resolve races Resolve.
    // The mutex is the mechanism; the assertions are timing-independent: every
    // thread must resolve, and the fetch must have happened exactly once.
    CountingHook hook;
    hook.models = {{"big", 131072, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    std::vector<std::thread> threads;
    std::atomic<int> resolved{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            if (config.Resolve(ModelRef{"local", "big"}) != nullptr) ++resolved;
        });
    }
    for (auto& t : threads) t.join();
    CHECK(resolved.load() == 8);
    CHECK(hook.calls == 1);  // one fetch, not eight
}

// --- plugin extensions: entry translators and secret sources ----------------------

namespace {

struct RenameTranslator final : ProviderEntryTranslator {
    void Translate(nlohmann::json& entry, const std::string&) const override {
        if (entry.contains("endpoint")) {
            entry["url"] = entry.at("endpoint");
            entry.erase("endpoint");
        }
    }
};

struct FixedSource final : SecretSource {
    std::string Describe() const override { return "fixed source"; }
    std::optional<SecretEntry> Lookup(const std::string& name) const override {
        if (name != "remote") return std::nullopt;
        return SecretEntry{"plugin-key", {}};
    }
};

}  // namespace

TEST_CASE("a plugin translator rewrites an entry before core parses it", "[providers][extensions]") {
    const nlohmann::json doc = nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "endpoint": "http://x/v1", "models": {"m": {}}}}
    })");
    // Core alone does not know the foreign key: the entry lacks a url.
    CHECK_THROWS(ParseProvidersConfig(doc));

    ProviderConfigExtensions extensions;
    extensions.AddTranslator(std::make_shared<RenameTranslator>());
    const auto config = ParseProvidersConfig(doc, {}, &extensions);
    const auto model = config.Resolve(ModelRef{"local", "m"});
    REQUIRE(model != nullptr);
    CHECK(model->provider.base_url == "http://x/v1");
}

TEST_CASE("a plugin secret source answers only its own namespace", "[providers][extensions]") {
    ProviderConfigExtensions extensions;
    extensions.SetAttribution("fixture");
    extensions.AddSecretSource(std::make_shared<FixedSource>());
    extensions.ClearAttribution();

    const auto key_for = [&](const std::string& ref) {
        nlohmann::json doc = nlohmann::json::parse(R"({
          "providers": {"remote": {"backend": "openai", "url": "http://x/v1", "models": {"m": {}}}}})");
        doc["providers"]["remote"]["api_key"] = ref;
        return ParseProvidersConfig(doc, {}, &extensions)
            .Resolve(ModelRef{"remote", "m"})
            ->provider.api_key;
    };

    const auto key = key_for("secret:fixture:remote").Get();
    CHECK(key.value == "plugin-key");
    CHECK(key.source == "fixture: fixed source");

    // A name the source does not have, and a namespace no plugin registered:
    // both bind (the source may answer later) and fail at use, by name.
    CHECK_THROWS_WITH(key_for("secret:fixture:other").Get(),
                      Catch::Matchers::ContainsSubstring("has no secret 'other'"));
    CHECK_THROWS_WITH(key_for("secret:elsewhere:remote").Get(),
                      Catch::Matchers::ContainsSubstring("no loaded plugin 'elsewhere'"));

    // A source registered outside a plugin's registration window has no
    // namespace, so it is refused rather than answering for everyone.
    ProviderConfigExtensions unattributed;
    unattributed.AddSecretSource(std::make_shared<FixedSource>());
    CHECK(unattributed.FindSecretSource("") == nullptr);
}

TEST_CASE("openrouter_app_id synthesizes the attribution headers on every request surface",
          "[providers][attribution]") {
    // The opt-in ("openrouter_app_id": true) must reach both places the
    // openai backend reads headers from: the resolved static model (chat
    // requests) and the ProviderEntry's connection (shared with /models
    // discovery), so attributed usage is not undercut by unattributed
    // discovery fetches.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "closedrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "openrouter_app_id": true,
          "models": {"glm": {}}
        }
      }
    })"));

    const auto model = config.Resolve(ModelRef{"closedrouter", "glm"});
    REQUIRE(model != nullptr);
    REQUIRE(model->provider.headers.size() == 2);
    bool saw_title = false, saw_referer = false;
    for (const auto& [name, value] : model->provider.headers) {
        if (name == "X-Title") {
            saw_title = true;
            CHECK(value == "minimal");
        }
        if (name == "HTTP-Referer") {
            saw_referer = true;
            CHECK(value == "https://github.com/lim8en1/minimal");
        }
    }
    CHECK(saw_title);
    CHECK(saw_referer);

    const auto& entry = config.provider_entries().at("closedrouter");
    REQUIRE(entry.connection.headers.size() == 2);
}

TEST_CASE("explicit headers win over the openrouter_app_id synthesis, per name",
          "[providers][attribution]") {
    // The native-keys-always-win rule: an operator who sets their own X-Title
    // (rebranding) keeps it and still receives the synthesized HTTP-Referer.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "closedrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "openrouter_app_id": true,
          "headers": {"X-Title": "my-own-app"},
          "models": {"glm": {}}
        }
      }
    })"));

    const auto model = config.Resolve(ModelRef{"closedrouter", "glm"});
    REQUIRE(model != nullptr);
    REQUIRE(model->provider.headers.size() == 2);
    for (const auto& [name, value] : model->provider.headers) {
        if (name == "X-Title") CHECK(value == "my-own-app");
        if (name == "HTTP-Referer") CHECK(value == "https://github.com/lim8en1/minimal");
    }
}

TEST_CASE("openrouter_app_id false or absent synthesizes nothing",
          "[providers][attribution]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "closedrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "openrouter_app_id": false,
          "models": {"glm": {}}
        },
        "plain": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "models": {"glm": {}}
        }
      }
    })"));

    CHECK(config.Resolve(ModelRef{"closedrouter", "glm"})->provider.headers.empty());
    CHECK(config.Resolve(ModelRef{"plain", "glm"})->provider.headers.empty());
}

TEST_CASE("a non-boolean openrouter_app_id is a named parse error", "[providers][attribution]") {
    // "app_id" suggests a string; a typo'd value must fail HERE, not surface
    // as an unexplained missing header on the wire.
    CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "closedrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "openrouter_app_id": "minimal",
          "models": {"glm": {}}
        }
      }
    })")),
                    std::runtime_error);
}

TEST_CASE("a model's non-object options is dropped with a warning, not a parse failure",
          "[providers][compat]") {
    testing::TempDir dir;
    std::ofstream(dir.path() / "providers.json") << R"({
      "providers": {
        "openrouter": {
          "backend": "openai",
          "url": "https://openrouter.ai/api/v1",
          "models": {"glm": {"options": "bogus"}}
        }
      }
    })json";

    // The warning itself rides the process logger; the OBSERVABLE contract is
    // that the malformed entry degrades to a parseable model with no
    // parameters instead of failing the whole config.
    const auto config = LoadProvidersConfig(dir.path() / "providers.json");
    const auto glm = config.Resolve(ModelRef{"openrouter", "glm"});
    REQUIRE(glm != nullptr);
    CHECK(glm->parameters.empty());
}

TEST_CASE("ModelRefs honors blacklist and whitelist as a listing filter only",
          "[providers][compat]") {
    testing::TempDir dir;
    std::ofstream(dir.path() / "providers.json") << R"({
      "providers": {
        "anthropic": {
          "backend": "openai",
          "url": "https://api.anthropic.com/v1",
          "whitelist": ["claude-sonnet"],
          "models": {
            "claude-sonnet": {},
            "claude-opus": {},
            "claude-haiku": {"blacklisted": true}
          }
        },
        "openai": {
          "backend": "openai",
          "url": "https://api.openai.com/v1",
          "blacklist": ["gpt-4o"],
          "models": {"gpt-4o": {}, "gpt-4o-mini": {}}
        }
      }
    })";
    // The blacklist syntax lives on the provider, not a model; move haiku out.
    std::ofstream(dir.path() / "providers.json") << R"({
      "providers": {
        "anthropic": {
          "backend": "openai",
          "url": "https://api.anthropic.com/v1",
          "whitelist": ["claude-sonnet"],
          "models": {"claude-sonnet": {}, "claude-opus": {}}
        },
        "openai": {
          "backend": "openai",
          "url": "https://api.openai.com/v1",
          "blacklist": ["gpt-4o"],
          "models": {"gpt-4o": {}, "gpt-4o-mini": {}}
        }
      }
    })";

    const auto config = LoadProvidersConfig(dir.path() / "providers.json");
    const auto refs = config.ModelRefs();
    // Filtered listing: anthropic shows only the whitelisted model; openai's
    // blacklisted one is hidden, its sibling stays.
    CHECK(std::find(refs.begin(), refs.end(), "anthropic/claude-sonnet") != refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "anthropic/claude-opus") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "openai/gpt-4o") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "openai/gpt-4o-mini") != refs.end());
    // Filtering is a LISTING concern: an explicit resolve still works.
    CHECK(config.Resolve(ModelRef{"anthropic", "claude-opus"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"openai", "gpt-4o"}) != nullptr);
}

TEST_CASE("a secret: api_key reads the operator's store per use, not once at load",
          "[providers][secrets]") {
    testing::TempDir dir;
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    std::ofstream(dir.path() / "providers.json") << R"({
      "providers": {"local": {"backend": "openai", "url": "http://x/v1",
                               "api_key": "secret:local-key", "models": {"m": {}}}}
    })";

    // Not stored yet: the entry still loads (a warning), and the use fails
    // naming the command that fixes it.
    const auto config = LoadProvidersConfig(dir.path() / "providers.json");
    const auto model = config.Resolve(ModelRef{"local", "m"});
    REQUIRE(model != nullptr);
    CHECK_THROWS_WITH(model->provider.api_key.Get(),
                      Catch::Matchers::ContainsSubstring("/secret set local-key"));

    // Stored after load: the SAME handle answers -- no restart.
    std::filesystem::create_directories(data.path() / "minimal");
    std::ofstream(data.path() / "minimal" / "secrets.json")
        << R"({"local-key": {"value": "sk-stored"}})";
    const auto key = model->provider.api_key.Get();
    CHECK(key.value == "sk-stored");
    CHECK(key.source == (data.path() / "minimal" / "secrets.json").string());
    CHECK(model->provider.api_key.description() == "secret:local-key");

    // The ProviderEntry connection (discovery's) carries the same handle.
    CHECK(config.provider_entries().at("local").connection.api_key.Get().value == "sk-stored");
}

TEST_CASE("a host-bound secret is refused for any other destination", "[providers][secrets]") {
    // The pasted-entry exploit shape: an entry redirects `url` to an
    // attacker's host and names the operator's real key. A host binding on
    // the secret refuses to send it there.
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    std::filesystem::create_directories(data.path() / "minimal");
    std::ofstream(data.path() / "minimal" / "secrets.json")
        << R"({"anthropic": {"value": "real-secret-key", "hosts": ["api.anthropic.com"]}})";

    const auto key_for = [](const std::string& url) {
        nlohmann::json doc = nlohmann::json::parse(R"({
          "providers": {"a": {"backend": "anthropic", "api_key": "secret:anthropic",
                               "models": {"m": {}}}}})");
        doc["providers"]["a"]["url"] = url;
        return ParseProvidersConfig(doc).Resolve(ModelRef{"a", "m"})->provider.api_key;
    };
    CHECK(key_for("https://api.anthropic.com").Get().value == "real-secret-key");
    CHECK_THROWS_WITH(key_for("http://evil.example/v1").Get(),
                      Catch::Matchers::ContainsSubstring("may not be sent to evil.example"));
}

TEST_CASE("a keyless entry never picks up a stored secret implicitly", "[providers][secrets]") {
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    std::filesystem::create_directories(data.path() / "minimal");
    std::ofstream(data.path() / "minimal" / "secrets.json")
        << R"({"anthropic": {"value": "real-secret-key"}})";
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"anthropic": {"backend": "openai", "url": "http://evil.example/v1",
                                   "models": {"m": {}}}}
    })"));
    CHECK_FALSE(config.Resolve(ModelRef{"anthropic", "m"})->provider.api_key.configured());
}

TEST_CASE("small_model parses from settings.json", "[settings][compat]") {
    testing::TempDir dir;
    std::ofstream(dir.path() / "settings.json") << R"({"small_model": "main/fast"})";
    const auto settings = LoadSettingsConfig(dir.path() / "settings.json");
    CHECK(settings.small_model == "main/fast");

    std::ofstream(dir.path() / "empty.json") << "{}";
    CHECK(LoadSettingsConfig(dir.path() / "empty.json").small_model.empty());
}

// --- Reasoning-effort variants -----------------------------------------------

TEST_CASE("a variant name outside the known efforts is refused where the mistake is",
          "[providers][variants]") {
    try {
        ParseProvidersConfig(nlohmann::json::parse(R"({
          "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                              "models": {"m": {"variants": ["hight"]}}}}
        })"));
        FAIL("expected the unknown effort name to be refused");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("hight") != std::string::npos);
        CHECK(message.find("not a known effort") != std::string::npos);
        // The valid vocabulary is in the message -- the file's own rule that a
        // refusal teaches the schema.
        CHECK(message.find("low") != std::string::npos);
        CHECK(message.find("xhigh") != std::string::npos);
    }
}

TEST_CASE("a variants value that is neither array nor object is refused by name",
          "[providers][variants]") {
    try {
        ParseProvidersConfig(nlohmann::json::parse(R"({
          "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                              "variants": true,
                              "models": {"m": {}}}}
        })"));
        FAIL("expected the boolean variants to be refused");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find("\"variants\" must be an array of effort names "
                                             "or an object") != std::string::npos);
    }
}

TEST_CASE("list-form variants synthesize addressable siblings with backend-derived parameters",
          "[providers][variants]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "anthropic": {"backend": "anthropic", "url": "https://api.anthropic.com",
                      "max_tokens": 32000,
                      "models": {"claude": {"context_window_tokens": 200000,
                                            "variants": ["low", "xhigh"]}}},
        "openai": {"backend": "openai", "url": "http://x/v1",
                   "models": {"gpt": {"variants": ["medium"]}}}
      }
    })"), AnthropicVariantParamsHookFixture());

    // The base still resolves, and each declared name became a sibling.
    const auto base = Require(config, "anthropic/claude");
    CHECK(base.provider.model_name == "claude");

    const auto& low = Require(config, "anthropic/claude@low");
    // THE WIRE ID STAYS THE BASE'S: agent_loop sends provider.model_name
    // verbatim, so a suffixed one would 400 everywhere.
    CHECK(low.provider.model_name == "claude");
    REQUIRE(low.parameters.contains("thinking"));
    CHECK(low.parameters.at("thinking").at("type") == "enabled");
    CHECK(low.parameters.at("thinking").at("budget_tokens") == 4096);
    // Variant-scoped fields the variant did not carry ride through from the base.
    REQUIRE(low.context_window_tokens.has_value());
    CHECK(*low.context_window_tokens == 200000);
    REQUIRE(low.max_tokens.has_value());
    CHECK(*low.max_tokens == 32000);

    // xhigh maps to the top budget tier of the anthropic scale.
    const auto& xhigh = Require(config, "anthropic/claude@xhigh");
    CHECK(xhigh.parameters.at("thinking").at("budget_tokens") == 16384);

    // The openai backend derives the effort word; xhigh/max normalize to high.
    const auto& medium = Require(config, "openai/gpt@medium");
    CHECK(medium.parameters.at("reasoning_effort") == "medium");
    CHECK_FALSE(medium.parameters.contains("thinking"));
}

TEST_CASE("the anthropic thinking budget is clamped to fit the model's max_tokens",
          "[providers][variants][invariant]") {
    // The API rejects budget_tokens >= max_tokens; the parser sees both
    // numbers and must not let a guaranteed-400 config past load.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "anthropic", "url": "https://api.anthropic.com",
                          "max_tokens": 4096,
                          "models": {"m": {"variants": ["xhigh"]}}}}
    })"), AnthropicVariantParamsHookFixture());
    const auto& model = Require(config, "p/m@xhigh");
    // 16384 wanted, 4096 - 1024 available.
    CHECK(model.parameters.at("thinking").at("budget_tokens") == 3072);
}

TEST_CASE("VariantParametersFor: a registered hook produces identical output to the "
          "pre-relocation anthropic branch, across the full effort ladder",
          "[providers][variants]") {
    const auto hook = AnthropicVariantParamsHookFixture();

    {
        const auto [params, clamped] = VariantParametersFor("anthropic", "low", 32000, hook);
        CHECK_FALSE(clamped);
        CHECK(params.at("thinking").at("type") == "enabled");
        CHECK(params.at("thinking").at("budget_tokens") == 4096);
    }
    {
        const auto [params, clamped] = VariantParametersFor("anthropic", "medium", 32000, hook);
        CHECK_FALSE(clamped);
        CHECK(params.at("thinking").at("budget_tokens") == 8192);
    }
    {
        const auto [params, clamped] = VariantParametersFor("anthropic", "high", 32000, hook);
        CHECK_FALSE(clamped);
        CHECK(params.at("thinking").at("budget_tokens") == 16384);
    }
    {
        // xhigh/max fold to "high" before the hook is ever consulted.
        const auto [params, clamped] = VariantParametersFor("anthropic", "xhigh", 32000, hook);
        CHECK(params.at("thinking").at("budget_tokens") == 16384);
        const auto [params2, clamped2] = VariantParametersFor("anthropic", "max", 32000, hook);
        CHECK(params2.at("thinking").at("budget_tokens") == 16384);
        (void)clamped;
        (void)clamped2;
    }
    {
        // Anthropic: thinking omitted entirely IS off.
        const auto [params, clamped] = VariantParametersFor("anthropic", "none", 32000, hook);
        CHECK_FALSE(clamped);
        CHECK(params.empty());
    }
    {
        // "minimal" has no Anthropic reading.
        CHECK_THROWS_AS(VariantParametersFor("anthropic", "minimal", 32000, hook),
                        std::runtime_error);
    }
}

TEST_CASE("VariantParametersFor: an unregistered kind (no hook, or a hook that answers "
          "nullopt) falls back to the generic reasoning_effort shape",
          "[providers][variants]") {
    // No hook at all -- the "provider-anthropic plugin isn't loaded" case.
    // Resolution never throws and never produces the old Anthropic shape;
    // only the value of an unused parameter blob differs (session
    // construction fails first at ProviderFactoryRegistry::Find("anthropic"),
    // before any request carrying these parameters is ever sent).
    {
        const auto [params, clamped] = VariantParametersFor("anthropic", "high", 32000);
        CHECK_FALSE(clamped);
        CHECK(params.at("reasoning_effort") == "high");
        CHECK_FALSE(params.contains("thinking"));
    }
    {
        const auto [params, clamped] = VariantParametersFor("anthropic", "none", 32000);
        CHECK_FALSE(clamped);
        CHECK(params.at("reasoning_effort") == "minimal");
    }
    // A hook that is set but has nothing registered for this kind -- same
    // fallback as no hook at all.
    {
        const VariantParamsHook empty_hook =
            [](const std::string&, const std::string&,
              std::optional<int>) -> std::optional<std::pair<nlohmann::json, bool>> {
            return std::nullopt;
        };
        const auto [params, clamped] = VariantParametersFor("anthropic", "high", 32000, empty_hook);
        CHECK_FALSE(clamped);
        CHECK(params.at("reasoning_effort") == "high");
    }
}

TEST_CASE("map-form variants carry full bodies; unknown keys are refused",
          "[providers][variants]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {"parameters": {"temperature": 0.5},
                                           "variants": {
                                             "deep": {"parameters": {"reasoning": {"effort": "high",
                                                                                   "max_tokens": 32768}}},
                                             "lean": {"parameters": {"reasoning_effort": "low"},
                                                      "max_tokens": 16384,
                                                      "context_window_tokens": 65536}}}}}}
    })"));

    const auto& deep = Require(config, "p/m@deep");
    // The variant's parameters overlay the base's per key.
    CHECK(deep.parameters.at("temperature") == 0.5);
    CHECK(deep.parameters.at("reasoning").at("effort") == "high");
    const auto& lean = Require(config, "p/m@lean");
    REQUIRE(lean.max_tokens.has_value());
    CHECK(*lean.max_tokens == 16384);
    REQUIRE(lean.context_window_tokens.has_value());
    CHECK(*lean.context_window_tokens == 65536);

    try {
        ParseProvidersConfig(nlohmann::json::parse(R"({
          "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                              "models": {"m": {"variants": {"bad": {"temp": 1}}}}}}
        })"));
        FAIL("expected the unknown variant key to be refused");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("bad") != std::string::npos);
        CHECK(message.find("temp") != std::string::npos);
    }
}

TEST_CASE("a model's own variants replace the provider-level recipe",
          "[providers][variants]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "variants": ["low"],
                          "models": {"m": {"variants": ["high"]},
                                     "n": {}}}}
    })"));
    // m: its own list only.
    CHECK(config.Resolve(ModelRef{"p", "m@high"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"p", "m@low"}) == nullptr);
    // n: inherits the provider's.
    CHECK(config.Resolve(ModelRef{"p", "n@low"}) != nullptr);
    // Variant names must not collide across the inheritance boundary: a
    // model-level recipe REPLACES, so the provider's names are gone for m.
}

TEST_CASE("a variant ref with an unknown base resolves to nothing, and a literal '@' id wins",
          "[providers][variants]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {"variants": ["low"]},
                                     "we@ird": {}}}}
    })"));
    CHECK(config.Resolve(ModelRef{"p", "ghost@low"}) == nullptr);
    CHECK(config.Resolve(ModelRef{"p", "m@high"}) == nullptr);  // undeclared variant: a miss
    // A hand-written id containing '@' (no live catalog serves one, but the
    // file may carry it) resolves verbatim -- the suffix split is attempted
    // first, and the literal-id fallback keeps it honest.
    CHECK(config.Resolve(ModelRef{"p", "we@ird"}) != nullptr);
}

TEST_CASE("VariantsOf lists declared siblings; a variant ref lists nothing",
          "[providers][variants]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {"variants": ["low", "medium", "xhigh"]}}}}
    })"));
    const auto names = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "low");
    CHECK(names[1] == "medium");
    CHECK(names[2] == "xhigh");
    CHECK(config.VariantsOf(ModelRef{"p", "m@low"}).empty());
    CHECK(config.VariantsOf(ModelRef{"p", "ghost"}).empty());
}

TEST_CASE("whitelist and blacklist match a variant ref by its BASE id",
          "[providers][variants][invariant]") {
    // THE LISTING RULE: hiding a model hides its variants; whitelisting a
    // model keeps them. Otherwise a variants-heavy config would need to list
    // every sibling by hand in the filters.
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "w": {"backend": "openai", "url": "http://x/v1", "whitelist": ["m"],
              "models": {"m": {"variants": ["low"]}, "other": {}}},
        "b": {"backend": "openai", "url": "http://x/v1", "blacklist": ["m"],
              "models": {"m": {"variants": ["low"]}, "other": {}}}
      }
    })"));
    const auto refs = config.ModelRefs();
    // whitelist ["m"]: m and its variant stay, everything else on the entry is
    // hidden from the LISTING (resolution is untouched -- the filter's own doc).
    CHECK(std::find(refs.begin(), refs.end(), "w/m") != refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "w/m@low") != refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "w/other") == refs.end());
    // blacklist ["m"]: m and its variant are hidden, the sibling stays.
    CHECK(std::find(refs.begin(), refs.end(), "b/m") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "b/m@low") == refs.end());
    CHECK(std::find(refs.begin(), refs.end(), "b/other") != refs.end());
}

TEST_CASE("discovery applies the entry recipe to models that advertise an effort ladder",
          "[providers][variants][discover]") {
    // glm-5.3-flash's live OpenRouter shape (2025-09): a reasoning object with
    // supported_efforts, riding DiscoveredModel::metadata verbatim.
    CountingHook hook;
    hook.models = {
        {"z-ai/glm-5.3-flash", 1310720,
         nlohmann::json::parse(
             R"({"reasoning": {"mandatory": true, "default_enabled": true,
                                "supported_efforts": ["max", "high", "low"]}})"),
         131072},
        {"no-reasoning", 8192, nlohmann::json::object(), std::nullopt},
        {"toggle-only", 8192, nlohmann::json::parse(R"({"reasoning": {"mandatory": true}})"),
         std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    // Default recipe: the model's OWN advertised ladder, verbatim --
    // ["max","high","low"] here, autodetected with no config. The wire id
    // stays the base's, and models without a ladder get nothing.
    const auto& base = config.Resolve(ModelRef{"local", "z-ai/glm-5.3-flash"});
    REQUIRE(base != nullptr);
    CHECK(base->provider.model_name == "z-ai/glm-5.3-flash");  // wire id unsuffixed
    const auto& high = config.Resolve(ModelRef{"local", "z-ai/glm-5.3-flash@high"});
    REQUIRE(high != nullptr);
    CHECK(high->provider.model_name == "z-ai/glm-5.3-flash");
    CHECK(high->parameters.at("reasoning_effort") == "high");
    // The ladder's own words become the names; each derives the openai
    // spelling at resolve time (max normalizes to high on the wire).
    const auto& max = config.Resolve(ModelRef{"local", "z-ai/glm-5.3-flash@max"});
    REQUIRE(max != nullptr);
    CHECK(max->parameters.at("reasoning_effort") == "high");
    CHECK(config.Resolve(ModelRef{"local", "z-ai/glm-5.3-flash@low"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"local", "no-reasoning@high"}) == nullptr);
    CHECK(config.Resolve(ModelRef{"local", "toggle-only@high"}) == nullptr);

    // The listing (VariantsOf, cold-cache-safe after the resolves above warmed
    // it) names the whole autodetected ladder, in catalog order.
    const auto names = config.VariantsOf(ModelRef{"local", "z-ai/glm-5.3-flash"});
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "max");
    CHECK(names[1] == "high");
    CHECK(names[2] == "low");
    CHECK(config.VariantsOf(ModelRef{"local", "no-reasoning"}).empty());
}

TEST_CASE(
    "discovery skips an unknown effort name instead of publishing an inert variant",
    "[providers][variants][discover][regression]") {
    // The bug: an autodetected effort name outside IsKnownEffortName's
    // vocabulary was pushed into the variant ladder verbatim, with no
    // validation (unlike the config-file path, ParseVariants, which rejects
    // unknown names at parse time). The variant resolved successfully as an
    // addressable @<name>, but MakeVariantResolved's IsKnownEffortName guard
    // skipped parameter derivation for it -- a silently inert selector
    // sending a request identical to the base model, with no warning.
    CountingHook hook;
    hook.models = {{"m", 131072,
                    nlohmann::json::parse(
                        R"({"reasoning": {"supported_efforts": ["high", "turbo", "low"]}})"),
                    std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    REQUIRE(config.Resolve(ModelRef{"local", "m"}) != nullptr);  // warms the discovery cache

    // The unknown rung is skipped entirely, not published as an inert variant.
    CHECK(config.Resolve(ModelRef{"local", "m@turbo"}) == nullptr);
    const auto names = config.VariantsOf(ModelRef{"local", "m"});
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "high");
    CHECK(names[1] == "low");
    // The known rungs on either side of it still resolve normally.
    const auto& high = config.Resolve(ModelRef{"local", "m@high"});
    REQUIRE(high != nullptr);
    CHECK(high->parameters.at("reasoning_effort") == "high");
}

TEST_CASE(
    "an explicit empty variants list opts a model out, even when discovery advertises a ladder",
    "[providers][variants][discover][regression]") {
    // The bug: an explicit `variants: []` (an operator's deliberate opt-out)
    // and "no variants key at all" (defer to discovery) both parse to an
    // empty vector via ParseVariants, so VariantsOf/ResolveVariant could not
    // tell them apart -- the opt-out was silently overridden by the
    // autodetected ladder the very next lookup fell through to.
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "local": {"backend": "openai", "url": "http://localhost:8000/v1", "discover": true,
                  "models": {"m": {"variants": []}}}
      }
    })"));
    CountingHook hook;
    hook.models = {{"m", 131072,
                    nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["high", "low"]}})"),
                    std::nullopt}};
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    REQUIRE(config.Resolve(ModelRef{"local", "m"}) != nullptr);  // warms the discovery cache

    CHECK(config.VariantsOf(ModelRef{"local", "m"}).empty());
    CHECK(config.Resolve(ModelRef{"local", "m@high"}) == nullptr);
}

TEST_CASE("an entry-level variants recipe replaces the discovery default",
          "[providers][variants][discover]") {
    CountingHook hook;
    hook.models = {{"m1", 8192,
                    nlohmann::json::parse(R"({"reasoning": {"supported_efforts":
                                             ["max", "high", "low"]}})"),
                    std::nullopt}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "url": "http://localhost:8000/v1",
                              "discover": true, "variants": ["low", "max"]}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    CHECK(config.Resolve(ModelRef{"local", "m1@low"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"local", "m1@max"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"local", "m1@high"}) == nullptr);  // not in the recipe
}

TEST_CASE("a recipe name the catalog does not advertise still resolves -- the recipe is the operator's",
          "[providers][variants][discover]") {
    // The recipe is a statement about the BACKEND's spelling; the catalog
    // gates whether the knob exists at all, not which rungs an operator may
    // name. A model advertising a ladder with any efforts takes the recipe as
    // written.
    CountingHook hook;
    hook.models = {{"m1", 8192,
                    nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["low"]}})"),
                    std::nullopt}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"local": {"backend": "openai", "url": "http://localhost:8000/v1",
                              "discover": true, "variants": ["xhigh"]}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    CHECK(config.Resolve(ModelRef{"local", "m1@xhigh"}) != nullptr);
    CHECK(config.Resolve(ModelRef{"local", "m1@xhigh"})->parameters.at("reasoning_effort") ==
          "high");
}

TEST_CASE("a cold discovery cache contributes no variants to VariantsOf",
          "[providers][variants][discover]") {
    auto config = DiscoverConfig();  // hook never set: discovery would fail
    CHECK(config.VariantsOf(ModelRef{"local", "anything"}).empty());
}

TEST_CASE("a static twin without variants picks up the discovery twin's autodetected ladder",
          "[providers][variants][discover][invariant]") {
    // THE openrouter SHAPE: "discover": true plus a static entry for the same
    // model id (the static twin carries parameters the listing cannot express
    // -- provider routing -- and no variants of its own). The static shadow
    // wins for the base's identity, but the autodetected ladder still applies:
    // /think must list it and ResolveVariant must serve it from the STATIC
    // base (routing parameters included), not from the discovery twin that
    // would lose them. A static twin that DOES declare its own variants keeps
    // them -- explicit beats autodetection.
    CountingHook hook;
    hook.models = {
        {"m", 1310720,
         nlohmann::json::parse(
             R"({"reasoning": {"supported_efforts": ["max", "high", "low"]}})"),
         131072}};
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true,
                          "models": {"m": {"parameters": {"provider": {"only": ["z-ai"]}}}}}}
    })"));
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });

    const auto& base = config.Resolve(ModelRef{"p", "m"});
    REQUIRE(base != nullptr);
    CHECK(base->parameters.at("provider").at("only") == nlohmann::json::array({"z-ai"}));

    const auto& max = config.Resolve(ModelRef{"p", "m@max"});
    REQUIRE(max != nullptr);
    // Sibling of the STATIC twin: routing parameters ride, wire id unsuffixed.
    CHECK(max->parameters.at("provider").at("only") == nlohmann::json::array({"z-ai"}));
    CHECK(max->parameters.at("reasoning_effort") == "high");
    CHECK(max->provider.model_name == "m");
    // And the discovery twin's window/cap fill the static file's gaps -- a
    // session on the sibling gets the same compaction as one on the base.
    REQUIRE(max->context_window_tokens.has_value());
    CHECK(*max->context_window_tokens == 1310720);
    REQUIRE(max->max_tokens.has_value());
    CHECK(*max->max_tokens == 131072);

    const auto names = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "max");
    CHECK(names[1] == "high");
    CHECK(names[2] == "low");

    // Explicit beats autodetection: a static twin with its own list keeps it.
    CountingHook hook2 = hook;
    auto config2 = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "discover": true,
                          "models": {"m": {"variants": ["low"]}}}}
    })"));
    config2.SetDiscoveryHook([&hook2](const std::string& kind, const llm::ProviderConfig& c) {
        return hook2.AsHook(kind, c);
    });
    CHECK(config2.Resolve(ModelRef{"p", "m@low"}) != nullptr);
    CHECK(config2.Resolve(ModelRef{"p", "m@max"}) == nullptr);
    const auto names2 = config2.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names2.size() == 1);
    CHECK(names2[0] == "low");
}

TEST_CASE("known-effort helpers and the suffix split behave", "[providers][variants]") {
    CHECK(IsKnownEffortName("low"));
    CHECK(IsKnownEffortName("xhigh"));
    CHECK_FALSE(IsKnownEffortName("hight"));
    CHECK_FALSE(IsKnownEffortName(""));

    const auto [plain, empty] = SplitVariantSuffix("claude");
    CHECK(plain == "claude");
    CHECK(empty.empty());
    const auto [base, suffix] = SplitVariantSuffix("anthropic/claude@high");
    CHECK(base == "anthropic/claude");
    CHECK(suffix == "high");
    const auto [last, wins] = SplitVariantSuffix("a@b@low");
    CHECK(last == "a@b");
    CHECK(wins == "low");
    const auto [no_tail, none] = SplitVariantSuffix("m@");
    CHECK(no_tail == "m@");  // an empty tail is not a variant name
    CHECK(none.empty());
    const auto [no_head, _] = SplitVariantSuffix("@high");
    (void)_;
    CHECK(no_head == "@high");  // an empty base is not a variant address
}

// --- Metadata enrichment (docs/EXTENSIBILITY.md §2.13) -----------------------

namespace {

// A metadata hook with a call counter and a canned answer -- the metadata twin
// of CountingHook. `answers` is consulted per model id (missing id = nullopt),
// and every consultation is counted and recorded, so tests can assert on HOW
// OFTEN and WITH WHAT the catalog is asked.
struct CountingMetadataHook {
    int calls = 0;
    std::map<std::string, llm::DiscoveredModel> answers;
    std::vector<std::string> asked_ids;
    std::vector<llm::MetadataMatch> asked_matches;
    std::vector<std::string> asked_pins;

    std::optional<llm::DiscoveredModel> AsHook(const llm::MetadataQuery& query,
                                               llm::MetadataMatch match) {
        ++calls;
        asked_ids.push_back(query.model_id);
        asked_matches.push_back(match);
        asked_pins.push_back(query.metadata_provider);
        const auto it = answers.find(query.model_id);
        if (it == answers.end()) return std::nullopt;
        return it->second;
    }
};

// A non-discover entry with one static model missing BOTH the window and the
// cap -- the shape metadata enrichment exists for.
ProvidersConfig StaticConfig() {
    return ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {}}}}
    })"));
}

}  // namespace

TEST_CASE("metadata enrichment gap-fills a static model's window and cap",
          "[providers][metadata]") {
    auto config = StaticConfig();
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{"m", 200000, nlohmann::json::object(), 65536};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    const auto& resolved = config.Resolve(ModelRef{"p", "m"});
    REQUIRE(resolved != nullptr);
    REQUIRE(resolved->context_window_tokens.has_value());
    CHECK(*resolved->context_window_tokens == 200000);
    REQUIRE(resolved->max_tokens.has_value());
    CHECK(*resolved->max_tokens == 65536);
    // One consult, cached: a second resolve is a lookup, not a re-ask.
    REQUIRE(config.Resolve(ModelRef{"p", "m"}) != nullptr);
    CHECK(hook.calls == 1);
}

TEST_CASE("a metadata miss never fails a resolve and is not re-consulted",
          "[providers][metadata][invariant]") {
    auto config = StaticConfig();
    CountingMetadataHook hook;  // no answers at all
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    const auto& resolved = config.Resolve(ModelRef{"p", "m"});
    // The static model resolves as parsed -- a dead enrichment source must not
    // take a hand-written model down with it (the discovery degradation rule).
    REQUIRE(resolved != nullptr);
    CHECK_FALSE(resolved->context_window_tokens.has_value());
    CHECK(hook.calls == 1);  // tombstoned: the miss costs one consult, not one per resolve
    REQUIRE(config.Resolve(ModelRef{"p", "m"}) != nullptr);
    CHECK(hook.calls == 1);
}

TEST_CASE("user-declared fields beat metadata enrichment, field by field",
          "[providers][metadata][invariant]") {
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {"context_window_tokens": 4096}}}}
    })"));
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{"m", 999999, nlohmann::json::object(), 999};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    const auto& resolved = config.Resolve(ModelRef{"p", "m"});
    REQUIRE(resolved != nullptr);
    CHECK(*resolved->context_window_tokens == 4096);  // declared window stands
    REQUIRE(resolved->max_tokens.has_value());
    CHECK(*resolved->max_tokens == 999);  // undeclared cap still fills
}

TEST_CASE("metadata_match carries the entry's policy and the provider pin rides the query",
          "[providers][metadata]") {
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "metadata_match": "substring", "metadata_provider": "anthropic",
                          "models": {"m": {}}}}
    })"));
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{"m", 1000, nlohmann::json::object(), std::nullopt};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    REQUIRE(config.Resolve(ModelRef{"p", "m"}) != nullptr);
    REQUIRE(hook.calls == 1);
    CHECK(hook.asked_matches.front() == llm::MetadataMatch::Substring);
    CHECK(hook.asked_pins.front() == "anthropic");
    CHECK(hook.asked_ids.front() == "m");
}

// No double quotes in the name: catch_discover_tests hands it to ctest as a
// filter, and the quoting does not survive the round trip on Linux ("No test
// cases matched").
TEST_CASE("metadata=false declines enrichment for the entry", "[providers][metadata]") {
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1", "metadata": false,
                          "models": {"m": {}}}}
    })"));
    CHECK(config.provider_entries().at("p").metadata == false);
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{"m", 200000, nlohmann::json::object(), 65536};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    REQUIRE(config.Resolve(ModelRef{"p", "m"}) != nullptr);
    CHECK(hook.calls == 0);  // never consulted
    CHECK_FALSE(config.Resolve(ModelRef{"p", "m"})->context_window_tokens.has_value());
}

TEST_CASE("a static model gains its autodetected ladder from metadata enrichment",
          "[providers][metadata][variants]") {
    auto config = StaticConfig();
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{
        "m", 200000,
        nlohmann::json::parse(
            R"({"reasoning_options": [{"type": "effort", "values": ["low", "high"]}]})"),
        65536};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    // Cold, the listing shows nothing (VariantsOf is not a consultation
    // trigger -- the discovery rule at "a cold discovery cache contributes no
    // variants" applies here too).
    CHECK(config.VariantsOf(ModelRef{"p", "m"}).empty());

    const auto& base = config.Resolve(ModelRef{"p", "m"});
    REQUIRE(base != nullptr);
    // The first base resolve consults once and records the ladder: the
    // listing AND a sibling resolve serve it from cache from here on.
    const auto names = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "low");
    CHECK(names[1] == "high");

    const auto& high = config.Resolve(ModelRef{"p", "m@high"});
    REQUIRE(high != nullptr);
    CHECK(high->provider.model_name == "m");  // the wire id stays unsuffixed
    CHECK(high->parameters.at("reasoning_effort") == "high");
    REQUIRE(high->context_window_tokens.has_value());
    CHECK(*high->context_window_tokens == 200000);
    CHECK(hook.calls == 1);  // one consult covered base + sibling
}

TEST_CASE("declared variants beat the metadata ladder", "[providers][metadata][variants]") {
    auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {"p": {"backend": "openai", "url": "http://x/v1",
                          "models": {"m": {"variants": ["low"]}}}}
    })"));
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{
        "m", 200000,
        nlohmann::json::parse(
            R"({"reasoning_options": [{"type": "effort", "values": ["low", "max"]}]})"),
        65536};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    // The declared sibling is listed cold (parse-time synthesis), and the
    // metadata ladder never joins it: the resolve records an EMPTY recipe for
    // the base (explicit beats autodetection), so the listing stays as
    // parsed.
    const auto cold = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(cold.size() == 1);
    CHECK(cold[0] == "low");
    REQUIRE(config.Resolve(ModelRef{"p", "m@low"}) != nullptr);   // declared
    CHECK(config.Resolve(ModelRef{"p", "m@max"}) == nullptr);     // metadata: suppressed
    const auto names = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "low");
}

TEST_CASE("an unknown effort in the metadata ladder is skipped with the known set intact",
          "[providers][metadata][variants][regression]") {
    auto config = StaticConfig();
    CountingMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{
        "m", 200000,
        nlohmann::json::parse(R"({"reasoning_options": [{"type": "effort",
                                   "values": ["high", "turbo-9000"]}]})"),
        std::nullopt};
    config.SetMetadataHook(
        [&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) { return hook.AsHook(q, m); });

    CHECK(config.VariantsOf(ModelRef{"p", "m"}).empty());  // cold: not a trigger
    REQUIRE(config.Resolve(ModelRef{"p", "m"}) != nullptr);  // consults, records "high"
    const auto names = config.VariantsOf(ModelRef{"p", "m"});
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "high");
    CHECK(hook.calls == 1);
}

TEST_CASE("discovered models get their gaps filled from metadata at fetch time",
          "[providers][metadata][discover]") {
    auto config = DiscoverConfig();
    CountingHook discovery;  // a listing with no window, the vLLM gap
    discovery.models = {{"served1", std::nullopt, nlohmann::json::object(), std::nullopt}};
    CountingMetadataHook metadata;
    metadata.answers["served1"] =
        llm::DiscoveredModel{"served1", 262144, nlohmann::json::object(), 32768};
    config.SetDiscoveryHook([&discovery](const std::string& kind, const llm::ProviderConfig& c) {
        return discovery.AsHook(kind, c);
    });
    config.SetMetadataHook(
        [&metadata](const llm::MetadataQuery& q, llm::MetadataMatch m) {
            return metadata.AsHook(q, m);
        });

    // Trigger the lazy fetch through a resolve of the discovered id.
    const auto& resolved = config.Resolve(ModelRef{"local", "served1"});
    REQUIRE(resolved != nullptr);
    REQUIRE(resolved->context_window_tokens.has_value());
    CHECK(*resolved->context_window_tokens == 262144);
    REQUIRE(resolved->max_tokens.has_value());
    CHECK(*resolved->max_tokens == 32768);
    CHECK(metadata.asked_ids.size() == 1);
    CHECK(metadata.asked_ids.front() == "served1");
}

TEST_CASE("the listing wins: metadata never overwrites a discovered field",
          "[providers][metadata][discover][invariant]") {
    auto config = DiscoverConfig();
    CountingHook discovery;
    discovery.models = {{"served1", 8192, nlohmann::json::object(), 2048}};
    CountingMetadataHook metadata;
    metadata.answers["served1"] =
        llm::DiscoveredModel{"served1", 999999, nlohmann::json::object(), 999999};
    config.SetDiscoveryHook([&discovery](const std::string& kind, const llm::ProviderConfig& c) {
        return discovery.AsHook(kind, c);
    });
    config.SetMetadataHook(
        [&metadata](const llm::MetadataQuery& q, llm::MetadataMatch m) {
            return metadata.AsHook(q, m);
        });

    const auto& resolved = config.Resolve(ModelRef{"local", "served1"});
    REQUIRE(resolved != nullptr);
    CHECK(*resolved->context_window_tokens == 8192);
    CHECK(*resolved->max_tokens == 2048);
    // The metadata layer was never consulted: both gaps are already filled by
    // the listing, so there is nothing to ask it for.
    CHECK(metadata.calls == 0);
}

TEST_CASE("malformed metadata knobs are named parse errors", "[providers][metadata]") {
    for (const char* body : {
             R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                     "metadata": "yes", "models": {"m": {}}}}})",
             R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                     "metadata_match": "fuzzy", "models": {"m": {}}}}})",
             R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                     "metadata_provider": "", "models": {"m": {}}}}})",
         }) {
        CHECK_THROWS_AS(ParseProvidersConfig(nlohmann::json::parse(body)), std::runtime_error);
    }
}

TEST_CASE("unknown fields are named parse errors at every level", "[providers]") {
    using Catch::Matchers::ContainsSubstring;
    const auto parse = [](const char* body) {
        return ParseProvidersConfig(nlohmann::json::parse(body));
    };
    CHECK_THROWS_WITH(parse(R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                    "credentials": "secret:p", "models": {"m": {}}}}})"),
                      ContainsSubstring("\"credentials\" was removed"));
    CHECK_THROWS_WITH(parse(R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                    "api_key_env": "K", "models": {"m": {}}}}})"),
                      ContainsSubstring("\"api_key_env\" was removed"));
    CHECK_THROWS_WITH(parse(R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                    "apikey": "k", "models": {"m": {}}}}})"),
                      ContainsSubstring("provider 'p' has unknown field \"apikey\""));
    CHECK_THROWS_WITH(parse(R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                    "models": {"m": {"max_token": 5}}}}})"),
                      ContainsSubstring("model 'p/m' has unknown field \"max_token\""));
    CHECK_THROWS_WITH(parse(R"({"providers": {"p": {"backend": "openai", "url": "http://x/v1",
                                    "timeouts": {"connect": 5}, "models": {"m": {}}}}})"),
                      ContainsSubstring("timeouts has unknown field \"connect\""));
    CHECK_THROWS_WITH(parse(R"({"providers": {}, "provider": {}})"),
                      ContainsSubstring("unknown field \"provider\""));
}

TEST_CASE("comment keys are allowed wherever fields are", "[providers]") {
    const auto config = ParseProvidersConfig(nlohmann::json::parse(R"({
        "//": "top",
        "providers": {"//": "a comment among providers",
                      "p": {"//": "entry", "backend": "openai", "url": "http://x/v1",
                            "headers": {"//": "header", "X-A": "1"},
                            "models": {"//note": "a comment among models",
                                       "m": {"//": "model",
                                             "variants": {"//": "variant", "hi": {}}}}}}})"));
    const auto model = config.Resolve(ModelRef{"p", "m"});
    REQUIRE(model != nullptr);
    REQUIRE(model->provider.headers.size() == 1);
    CHECK(model->provider.headers[0].first == "X-A");
    CHECK(config.Resolve(ModelRef{"p", "//note"}) == nullptr);
}

// --- runtime concurrency characterization (extraction safety net) ------------------
//
// The runtime half of ProvidersConfig is called from RPC threads (ListModels),
// session workers, the command pool and the discovery warm-up thread at once.
// These pin the externally visible behaviour under that load BEFORE the runtime
// state is restructured: timing-independent assertions only, no sleeps for
// correctness. Hooks here are thread-safe (the older CountingHook relies on the
// per-provider gate to serialize its own bookkeeping).

namespace {

struct ThreadSafeDiscoveryHook {
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::vector<llm::DiscoveredModel> models;
    bool ok = true;
    std::string error = "boom";
    std::chrono::milliseconds delay{0};

    DiscoveryOutcome AsHook(const std::string&, const llm::ProviderConfig&) {
        ++calls;
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        DiscoveryOutcome outcome;
        outcome.catalog_present = true;
        std::lock_guard<std::mutex> lock(mutex);
        outcome.ok = ok;
        outcome.error = ok ? "" : error;
        outcome.models = models;
        return outcome;
    }
};

struct ThreadSafeMetadataHook {
    std::atomic<int> calls{0};
    std::map<std::string, llm::DiscoveredModel> answers;
    std::chrono::milliseconds delay{0};

    std::optional<llm::DiscoveredModel> AsHook(const llm::MetadataQuery& query,
                                               llm::MetadataMatch) {
        ++calls;
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const auto it = answers.find(query.model_id);
        if (it == answers.end()) return std::nullopt;
        return it->second;
    }
};

}  // namespace

TEST_CASE("RefreshDiscovery racing Resolve: every resolve succeeds, the end state is fresh",
          "[providers][discover][concurrency]") {
    ThreadSafeDiscoveryHook hook;
    hook.models = {{"big", 131072, nlohmann::json::object(), std::nullopt}};
    hook.delay = std::chrono::milliseconds(2);  // widen the race window
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& k, const llm::ProviderConfig& c) {
        return hook.AsHook(k, c);
    });

    std::atomic<int> null_resolves{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < 20; ++n) {
                if (config.Resolve(ModelRef{"local", "big"}) == nullptr) ++null_resolves;
            }
        });
    }
    threads.emplace_back([&] {
        for (int n = 0; n < 5; ++n) (void)config.RefreshDiscovery();
    });
    for (auto& t : threads) t.join();

    CHECK(null_resolves.load() == 0);
    CHECK(hook.calls.load() >= 1);
    CHECK(config.discovery_status("local").ok);
    const auto refs = config.ModelRefs();
    CHECK(std::count(refs.begin(), refs.end(), "local/big") == 1);
}

TEST_CASE("listings stay sorted and unique while discovery refreshes underneath",
          "[providers][discover][concurrency]") {
    ThreadSafeDiscoveryHook hook;
    hook.models = {{"a", 4096, nlohmann::json::object(), std::nullopt},
                   {"b", 4096, nlohmann::json::object(), std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& k, const llm::ProviderConfig& c) {
        return hook.AsHook(k, c);
    });
    REQUIRE(config.Resolve(ModelRef{"local", "a"}) != nullptr);  // warm

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};
    std::thread lister([&] {
        while (!stop.load()) {
            const auto refs = config.ModelRefs();
            if (!std::is_sorted(refs.begin(), refs.end())) ++violations;
            if (std::adjacent_find(refs.begin(), refs.end()) != refs.end()) ++violations;
            (void)config.discovery_status("local");
            (void)config.VariantsOf(ModelRef{"local", "a"});
        }
    });
    std::thread resolver([&] {
        for (int n = 0; n < 200; ++n) {
            (void)config.Resolve(ModelRef{"local", n % 2 ? "a" : "b"});
        }
    });
    for (int n = 0; n < 10; ++n) (void)config.RefreshDiscovery();
    resolver.join();
    stop = true;
    lister.join();

    CHECK(violations.load() == 0);
}

TEST_CASE("warm-up refresh and a first resolve of an unseen model both get an answer",
          "[providers][discover][concurrency]") {
    // The shape of main.cpp's discovery warm-up (RefreshDiscovery on its own
    // thread) racing the first /model switch to a not-yet-listed model.
    for (int round = 0; round < 10; ++round) {
        ThreadSafeDiscoveryHook hook;
        hook.models = {{"newmodel", 8192, nlohmann::json::object(), std::nullopt}};
        hook.delay = std::chrono::milliseconds(1);
        auto config = DiscoverConfig();
        config.SetDiscoveryHook([&hook](const std::string& k, const llm::ProviderConfig& c) {
            return hook.AsHook(k, c);
        });

        std::shared_ptr<const ResolvedModel> resolved;
        std::thread warmup([&] { (void)config.RefreshDiscovery(); });
        std::thread switcher([&] { resolved = config.Resolve(ModelRef{"local", "newmodel"}); });
        warmup.join();
        switcher.join();

        REQUIRE(resolved != nullptr);
        CHECK(*resolved->context_window_tokens == 8192);
        CHECK(config.discovery_status("local").ok);
    }
}

TEST_CASE("concurrent resolves of one static model consult metadata once",
          "[providers][metadata][concurrency]") {
    auto config = StaticConfig();
    ThreadSafeMetadataHook hook;
    hook.answers["m"] = llm::DiscoveredModel{"m", 32768, nlohmann::json::object(), 4096};
    hook.delay = std::chrono::milliseconds(5);  // keep the first consult in flight
    config.SetMetadataHook([&hook](const llm::MetadataQuery& q, llm::MetadataMatch m) {
        return hook.AsHook(q, m);
    });

    std::atomic<int> enriched{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            const auto resolved = config.Resolve(ModelRef{"p", "m"});
            if (resolved && resolved->context_window_tokens == 32768) ++enriched;
        });
    }
    for (auto& t : threads) t.join();

    // Every resolver sees the enriched model, and the catalog is asked once:
    // the others find the answer (or the tombstone) after waiting on the gate.
    CHECK(enriched.load() == 8);
    CHECK(hook.calls.load() == 1);
}

TEST_CASE("move-assign carries the runtime state: hooks, backoff, tombstones, ladders",
          "[providers][discover][move]") {
    ThreadSafeDiscoveryHook discovery;
    discovery.models = {{"big", 131072, nlohmann::json::object(), std::nullopt}};
    ThreadSafeMetadataHook metadata;  // no answers: the static model gets tombstoned

    ProvidersConfig source = ParseProvidersConfig(nlohmann::json::parse(R"({
      "providers": {
        "local": {"backend": "openai", "url": "http://localhost:8000/v1", "discover": true,
                  "max_tokens": 4096},
        "p": {"backend": "openai", "url": "http://x/v1", "models": {"m": {}}}
      }
    })"));
    source.SetDiscoveryHook([&](const std::string& k, const llm::ProviderConfig& c) {
        return discovery.AsHook(k, c);
    });
    source.SetMetadataHook([&](const llm::MetadataQuery& q, llm::MetadataMatch m) {
        return metadata.AsHook(q, m);
    });
    source.SetDiscoveryRetryBackoff(std::chrono::milliseconds(12345));
    REQUIRE(source.Resolve(ModelRef{"p", "m"}) != nullptr);      // tombstones "p/m"
    REQUIRE(metadata.calls.load() == 1);

    ProvidersConfig moved;
    moved = std::move(source);

    // The tombstone survived: no second consult.
    REQUIRE(moved.Resolve(ModelRef{"p", "m"}) != nullptr);
    CHECK(metadata.calls.load() == 1);
    // The discovery hook survived: a lazy fetch works on the moved-to object.
    const auto big = moved.Resolve(ModelRef{"local", "big"});
    REQUIRE(big != nullptr);
    CHECK(discovery.calls.load() == 1);
}

TEST_CASE("a discovered variant is derived through the injected variant-params hook",
          "[providers][variants][discover]") {
    ThreadSafeDiscoveryHook hook;
    hook.models = {{"thinker", 65536,
                    nlohmann::json::parse(R"({"reasoning": {"supported_efforts": ["high"]}})"),
                    std::nullopt}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& k, const llm::ProviderConfig& c) {
        return hook.AsHook(k, c);
    });
    std::atomic<int> hook_calls{0};
    config.SetVariantParamsHook([&hook_calls](const std::string& backend,
                                              const std::string& effort,
                                              std::optional<int>)
                                    -> std::optional<std::pair<nlohmann::json, bool>> {
        ++hook_calls;
        return std::make_pair(nlohmann::json{{"custom_effort", backend + ":" + effort}}, false);
    });

    const auto high = config.Resolve(ModelRef{"local", "thinker@high"});
    REQUIRE(high != nullptr);
    CHECK(hook_calls.load() >= 1);
    CHECK(high->parameters.at("custom_effort") == "openai:high");
}

TEST_CASE("a throwing discovery hook leaves the catalog usable", "[providers][discover]") {
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([](const std::string&, const llm::ProviderConfig&) -> DiscoveryOutcome {
        throw std::runtime_error("hook exploded");
    });
    // The exception reaches the caller...
    CHECK_THROWS_AS(config.Resolve(ModelRef{"local", "anything"}), std::runtime_error);
    // ...but neither the config lock nor the provider's gate stays held: every
    // other read still answers instead of deadlocking.
    CHECK_NOTHROW(config.ModelRefs());
    CHECK_NOTHROW(config.discovery_status("local"));
    CHECK_THROWS_AS(config.Resolve(ModelRef{"local", "anything"}), std::runtime_error);
}
