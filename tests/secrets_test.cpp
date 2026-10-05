// config/secrets.hpp: the reference syntax, the store document, host
// bindings. providers_config_test.cpp covers the same resolver as
// providers.json reaches it; this file covers it on its own.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

#include "minimal/config/provider_config_extensions.hpp"
#include "minimal/config/secrets.hpp"
#include "temp_dir.hpp"

using namespace minimal::config;
namespace testing = minimal::testing;
using Catch::Matchers::ContainsSubstring;

namespace {

void WriteStore(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << contents;
}

}  // namespace

TEST_CASE("HostAllowed matches host, and port only when the binding names one", "[secrets]") {
    CHECK(HostAllowed({}, "http://anything:1234/v1"));
    CHECK(HostAllowed({}, ""));

    const std::vector<std::string> hosts = {"api.example.com", "127.0.0.1:8080", "[::1]:9000"};
    CHECK(HostAllowed(hosts, "https://api.example.com/v1"));
    CHECK(HostAllowed(hosts, "https://API.Example.com:8443/v1"));  // case-folded, any port
    CHECK(HostAllowed(hosts, "http://127.0.0.1:8080/v1"));
    CHECK_FALSE(HostAllowed(hosts, "http://127.0.0.1:8081/v1"));
    CHECK_FALSE(HostAllowed(hosts, "http://127.0.0.1/v1"));  // implied :80
    CHECK(HostAllowed(hosts, "http://[::1]:9000/"));
    CHECK_FALSE(HostAllowed(hosts, "http://api.example.com.evil.test/"));
    // userinfo is not the host: the classic "trusted@evil" spelling.
    CHECK_FALSE(HostAllowed(hosts, "http://api.example.com@evil.test/"));
    CHECK(HostAllowed({"api.example.com:443"}, "https://api.example.com/v1"));  // implied :443
    // A bound secret is never sent where the destination cannot be checked.
    CHECK_FALSE(HostAllowed(hosts, ""));
}

TEST_CASE("ParseSecretStore skips malformed entries and refuses a malformed document",
          "[secrets]") {
    const auto entries = ParseSecretStore(R"({
      "good": {"value": "v1", "hosts": ["a.example", 7, ""]},
      "no-value": {"hosts": ["a.example"]},
      "not-an-object": "v2"
    })");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].first == "good");
    CHECK(entries[0].second.value == "v1");
    CHECK(entries[0].second.hosts == std::vector<std::string>{"a.example"});

    CHECK(ParseSecretStore("").empty());
    CHECK_THROWS_WITH(ParseSecretStore("{"), ContainsSubstring("malformed"));
    CHECK_THROWS_AS(ParseSecretStore("[]"), std::runtime_error);
}

TEST_CASE("Bind: empty, literal and env references", "[secrets]") {
    const SecretResolver resolver(std::filesystem::path("/nonexistent/secrets.json"));

    CHECK_FALSE(resolver.Bind("", "http://x").configured());

    const auto literal = resolver.Bind("sk-literal", "http://x");
    CHECK(literal.Get().value == "sk-literal");
    CHECK(literal.description() == "inline api_key");

    ::setenv("MINIMAL_TEST_SECRET_ENV", "from-env", 1);
    const auto env = resolver.Bind("env:MINIMAL_TEST_SECRET_ENV", "http://x");
    ::unsetenv("MINIMAL_TEST_SECRET_ENV");
    // Fixed at bind: the environment of a running process does not change.
    CHECK(env.Get().value == "from-env");
    CHECK(env.Get().source == "env MINIMAL_TEST_SECRET_ENV");

    CHECK_THROWS_WITH(resolver.Bind("env:MINIMAL_TEST_SECRET_ENV", "http://x"),
                      ContainsSubstring("unset"));
    CHECK_THROWS(resolver.Bind("env:", "http://x"));

    // Equality is the reference, never the value.
    CHECK(resolver.Bind("secret:a", "http://x") == resolver.Bind("secret:a", "http://y"));
    CHECK_FALSE(resolver.Bind("secret:a", "http://x") == resolver.Bind("secret:b", "http://x"));
    CHECK(minimal::common::SecretHandle{} == minimal::common::SecretHandle{});
    CHECK_FALSE(minimal::common::SecretHandle{} == resolver.Bind("secret:a", "http://x"));
}

TEST_CASE("Bind: a stored secret is read per Get, and its binding is enforced", "[secrets]") {
    testing::TempDir dir;
    const auto store = dir.path() / "secrets.json";
    const SecretResolver resolver(store);

    const auto handle = resolver.Bind("secret:emb", "http://127.0.0.1:8080/v1/embeddings");
    CHECK_THROWS_WITH(handle.Get(), ContainsSubstring("/secret set emb"));

    WriteStore(store, R"({"emb": {"value": "v1", "hosts": ["127.0.0.1:8080"]}})");
    CHECK(handle.Get().value == "v1");
    CHECK(handle.Get().source == store.string());

    WriteStore(store, R"({"emb": {"value": "v2", "hosts": ["127.0.0.1:9999"]}})");
    CHECK_THROWS_WITH(handle.Get(), ContainsSubstring("may not be sent to 127.0.0.1:8080"));

    WriteStore(store, R"({"emb": {"value": ""}})");
    CHECK_THROWS_WITH(handle.Get(), ContainsSubstring("is empty"));

    WriteStore(store, "{not json");
    CHECK_THROWS_WITH(handle.Get(), ContainsSubstring("malformed"));
}

namespace {

struct OneSecret final : SecretSource {
    std::string Describe() const override { return "one"; }
    std::optional<SecretEntry> Lookup(const std::string& name) const override {
        if (name != "k") return std::nullopt;
        return SecretEntry{"plugin-value", {"bound.example"}};
    }
};

}  // namespace

TEST_CASE("Bind: a plugin namespace reaches only that plugin's source", "[secrets]") {
    ProviderConfigExtensions extensions;
    const SecretResolver resolver(std::filesystem::path("/nonexistent/secrets.json"), &extensions);

    // Bound BEFORE the source registers -- the resolver exists before plugin
    // load in production -- and still answers once it does.
    const auto handle = resolver.Bind("secret:plug:k", "https://bound.example/x");
    CHECK_THROWS_WITH(handle.Get(), ContainsSubstring("no loaded plugin 'plug'"));

    extensions.SetAttribution("plug");
    extensions.AddSecretSource(std::make_shared<OneSecret>());
    extensions.AddSecretSource(std::make_shared<OneSecret>());  // second: ignored
    extensions.ClearAttribution();
    CHECK(extensions.OwnedBy("plug") == std::vector<std::string>{"secrets: one"});

    CHECK(handle.Get().value == "plugin-value");
    CHECK(handle.Get().source == "plug: one");
    // The source's own host binding applies too.
    CHECK_THROWS(resolver.Bind("secret:plug:k", "https://elsewhere.example/").Get());
    // The bare namespace is the operator's store, never a plugin's.
    CHECK_THROWS_WITH(resolver.Bind("secret:k", "https://bound.example/").Get(),
                      ContainsSubstring("/secret set k"));
}
