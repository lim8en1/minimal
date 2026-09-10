#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <cstdlib>
#include <fstream>
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

const ResolvedModel& Require(const ProvidersConfig& config, const std::string& text) {
    const auto ref = ParseModelRef(text);
    REQUIRE(ref.has_value());
    const auto model = config.Resolve(*ref);
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
          "api_key_env": "OPENROUTER_API_KEY",
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

TEST_CASE("a discover-only model resolves lazily, once", "[providers][discover]") {
    CountingHook hook;
    hook.models = {{"big", 131072, nlohmann::json::object()},
                   {"small", std::nullopt, nlohmann::json::object()}};
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
    CHECK(*big->max_tokens == 4096);  // provider-level default, inherited
    REQUIRE(big->context_window_tokens.has_value());
    CHECK(*big->context_window_tokens == 131072);

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
    hook.models = {{"zdisc", 4096, nlohmann::json::object()}};
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

TEST_CASE("a static model with an omitted window is enriched from discovery",
          "[providers][discover][invariant]") {
    // THE PAYOFF: compaction tracks the model actually served instead of a
    // number someone typed (or omitted, silently disabling compaction).
    // Resolving this window from the static entry alone fails this test.
    CountingHook hook;
    hook.models = {{"static1", 999999, nlohmann::json::object()}};
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
    hook.models = {{"small", std::nullopt, nlohmann::json::object()}};
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

TEST_CASE("RefreshDiscovery evicts and refetches, reporting per provider",
          "[providers][discover]") {
    // The mid-process drift case: the backend restarts serving a different
    // model set while the server stays up. Refresh makes the new set visible
    // without a server restart.
    CountingHook hook;
    hook.models = {{"old", 4096, nlohmann::json::object()}};
    auto config = DiscoverConfig();
    config.SetDiscoveryHook([&hook](const std::string& kind, const llm::ProviderConfig& c) {
        return hook.AsHook(kind, c);
    });
    REQUIRE(config.Resolve(ModelRef{"local", "old"}) != nullptr);

    hook.models = {{"new", 8192, nlohmann::json::object()}};
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
    hook.models = {{"big", 131072, nlohmann::json::object()}};
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

// --- opencode-compat provider surface ---------------------------------------------

TEST_CASE("an opencode-shaped provider entry normalizes onto minimal's fields",
          "[providers][compat]") {
    testing::TempDir dir;
    std::ofstream(dir.path() / "providers.json") << R"json({
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
    })json";

    const auto config = LoadProvidersConfig(dir.path() / "providers.json");
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

TEST_CASE("an opencode-shaped model's options fold into parameters, native keys winning "
          "per-key",
          "[providers][compat]") {
    // opencode's AI SDK spreads a model's `options` into the request body;
    // `parameters` is minimal's verbatim passthrough, so the fold makes an
    // opencode providers.json (e.g. OpenRouter's provider routing:
    // {"options": {"provider": {"only": ["z-ai/fp8"]}}}) work verbatim.
    testing::TempDir dir;
    std::ofstream(dir.path() / "providers.json") << R"json({
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
    })json";

    const auto config = LoadProvidersConfig(dir.path() / "providers.json");

    // Options only: folded over verbatim.
    const auto glm = config.Resolve(ModelRef{"openrouter", "glm"});
    REQUIRE(glm != nullptr);
    CHECK(glm->parameters == nlohmann::json{{"provider", {{"only", {"z-ai/fp8"}}}}});

    // Both: the native parameters keys win, the non-conflicting option stays.
    const auto kimi = config.Resolve(ModelRef{"openrouter", "kimi"});
    REQUIRE(kimi != nullptr);
    CHECK(kimi->parameters ==
          nlohmann::json{{"provider", {{"only", {"deepinfra/fp8"}}}}, {"temperature", 0.7}});
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

TEST_CASE("a missing api_key falls back to the credentials file ONLY when the entry opts "
          "in with credentials: auth-file (minimal's, then opencode's own)",
          "[providers][compat]") {
    testing::TempDir dir;

    // 1. <DataDir>/auth.json: XDG_DATA_HOME points at the parent of "minimal".
    {
        testing::TempDir data;
        ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
        const auto auth_dir = data.path() / "minimal";
        std::filesystem::create_directories(auth_dir);
        std::ofstream(auth_dir / "auth.json") << R"({"local": {"key": "minimal-stored"}})";
        std::ofstream(dir.path() / "providers.json") << R"({
          "providers": {"local": {"backend": "openai", "url": "http://x/v1",
                                   "credentials": "auth-file", "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers.json");
        const auto model = config.Resolve(ModelRef{"local", "m"});
        REQUIRE(model != nullptr);
        CHECK(model->provider.api_key == "minimal-stored");
    }

    // 2. opencode's own auth store, honored zero-copy -- still gated on opt-in.
    {
        testing::TempDir home;
        ScopedEnv home_env("HOME", home.path().string().c_str());
        const auto oc_dir = home.path() / ".local" / "share" / "opencode";
        std::filesystem::create_directories(oc_dir);
        std::ofstream(oc_dir / "auth.json") << R"({"remote": {"type": "api", "key": "oc-key"}})";
        std::ofstream(dir.path() / "providers2.json") << R"({
          "providers": {"remote": {"backend": "openai", "url": "http://x/v1",
                                    "credentials": "auth-file", "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers2.json");
        const auto model = config.Resolve(ModelRef{"remote", "m"});
        REQUIRE(model != nullptr);
        CHECK(model->provider.api_key == "oc-key");
    }
}

TEST_CASE("without the credentials: auth-file opt-in, a keyless entry resolves an empty "
          "key rather than silently pulling one from the shared auth store",
          "[providers][compat]") {
    // The B11 exploit shape: a pasted providers.json entry redirects `url` to
    // an attacker's host and supplies no key of its own. Before the opt-in
    // gate, this would silently pull the operator's REAL key out of
    // <data dir>/auth.json and send it to that url on the first request.
    testing::TempDir dir;
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    const auto auth_dir = data.path() / "minimal";
    std::filesystem::create_directories(auth_dir);
    std::ofstream(auth_dir / "auth.json") << R"({"anthropic": {"key": "real-secret-key"}})";
    // No "credentials" field, and a redirected url -- exactly the hostile shape.
    std::ofstream(dir.path() / "providers.json") << R"({
      "providers": {"anthropic": {"backend": "openai", "url": "http://evil.example/v1",
                                   "models": {"m": {}}}}
    })";

    const auto config = LoadProvidersConfig(dir.path() / "providers.json");
    const auto model = config.Resolve(ModelRef{"anthropic", "m"});
    REQUIRE(model != nullptr);
    CHECK(model->provider.api_key.empty());
}

TEST_CASE("api-key resolution records its source on the entry without carrying the key",
          "[providers][compat]") {
    // The startup log reads this origin (key material never reaches it), and
    // an auth-file opt-in that finds nothing is a DISTINCT recorded state --
    // a key written after the server started (e.g. via /connect) must be
    // visible here, not only as a 401 on the first request.
    testing::TempDir dir;
    testing::TempDir data;
    testing::TempDir home;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    ScopedEnv home_env("HOME", home.path().string().c_str());

    // 1. Opt-in, key present in <data dir>/auth.json: the origin names the
    //    file, never the key.
    {
        const auto auth_dir = data.path() / "minimal";
        std::filesystem::create_directories(auth_dir);
        std::ofstream(auth_dir / "auth.json") << R"({"local": {"key": "minimal-stored"}})";
        std::ofstream(dir.path() / "providers.json") << R"({
          "providers": {"local": {"backend": "openai", "url": "http://x/v1",
                                   "credentials": "auth-file", "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers.json");
        const auto& entry = config.provider_entries().at("local");
        CHECK(entry.connection.api_key == "minimal-stored");
        CHECK(entry.api_key_origin.find((auth_dir / "auth.json").string()) != std::string::npos);
        CHECK(entry.api_key_origin.find("minimal-stored") == std::string::npos);
        const auto model = config.Resolve(ModelRef{"local", "m"});
        REQUIRE(model != nullptr);
        CHECK(model->provider.api_key == "minimal-stored");
    }

    // 2. Inline key: named as inline, the value itself never carried.
    {
        std::ofstream(dir.path() / "providers.json") << R"({
          "providers": {"local": {"backend": "openai", "url": "http://x/v1",
                                   "api_key": "sekrit-inline", "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers.json");
        const auto& entry = config.provider_entries().at("local");
        CHECK(entry.connection.api_key == "sekrit-inline");
        CHECK(entry.api_key_origin == "inline api_key");
    }

    // 3. Opt-in, nothing found anywhere: the recorded miss, not a silent empty.
    {
        std::ofstream(dir.path() / "providers.json") << R"({
          "providers": {"openrouter": {"backend": "openai", "url": "https://openrouter.ai/api/v1",
                                        "credentials": "auth-file", "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers.json");
        const auto& entry = config.provider_entries().at("openrouter");
        CHECK(entry.connection.api_key.empty());
        CHECK(entry.api_key_origin == "auth-file (no key found)");
        const auto model = config.Resolve(ModelRef{"openrouter", "m"});
        REQUIRE(model != nullptr);
        CHECK(model->provider.api_key.empty());
    }

    // 4. No credential configured at all: the normal keyless local server --
    //    empty origin, and NOT the miss state.
    {
        std::ofstream(dir.path() / "providers.json") << R"({
          "providers": {"local": {"backend": "openai", "url": "http://x/v1",
                                   "models": {"m": {}}}}
        })";
        const auto config = LoadProvidersConfig(dir.path() / "providers.json");
        const auto& entry = config.provider_entries().at("local");
        CHECK(entry.connection.api_key.empty());
        CHECK(entry.api_key_origin.empty());
    }
}

TEST_CASE("small_model parses from settings.json", "[settings][compat]") {
    testing::TempDir dir;
    std::ofstream(dir.path() / "settings.json") << R"({"small_model": "main/fast"})";
    const auto settings = LoadSettingsConfig(dir.path() / "settings.json");
    CHECK(settings.small_model == "main/fast");

    std::ofstream(dir.path() / "empty.json") << "{}";
    CHECK(LoadSettingsConfig(dir.path() / "empty.json").small_model.empty());
}
