#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/mcp/env_interpolation.hpp"
#include "minimal/mcp/naming.hpp"

using minimal::mcp::InterpolateEnv;
using minimal::mcp::IsValidMcpServerName;
using minimal::mcp::IsValidMcpToolName;
using minimal::mcp::McpToolName;

namespace {

minimal::mcp::EnvLookup MapLookup(const std::map<std::string, std::string>& vars) {
    return [&vars](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

}  // namespace

TEST_CASE("McpToolName namespaces server and tool", "[mcp_naming]") {
    CHECK(McpToolName("ida", "list_instances") == "mcp__ida__list_instances");
    CHECK(McpToolName("a-b", "c.d") == "mcp__a-b__c.d");
}

TEST_CASE("IsValidMcpServerName rejects names that would corrupt the namespace", "[mcp_naming]") {
    CHECK(IsValidMcpServerName("ida"));
    CHECK(IsValidMcpServerName("ida-pro"));
    CHECK(IsValidMcpServerName("a_b"));
    CHECK_FALSE(IsValidMcpServerName(""));            // empty: mcp____tool
    CHECK_FALSE(IsValidMcpServerName("a__b"));        // separator inside: ambiguous parse
    CHECK_FALSE(IsValidMcpServerName("my server"));   // space
    CHECK_FALSE(IsValidMcpServerName("a\tb"));        // control
    CHECK_FALSE(IsValidMcpServerName("a\nb"));
}

TEST_CASE("IsValidMcpToolName uses the same rule", "[mcp_naming]") {
    CHECK(IsValidMcpToolName("list_instances"));
    CHECK_FALSE(IsValidMcpToolName("a__b"));
    CHECK_FALSE(IsValidMcpToolName(""));
}

TEST_CASE("InterpolateEnv resolves markers and passes everything else through", "[mcp_naming]") {
    const std::map<std::string, std::string> vars = {{"TOKEN", "s3cret"}, {"EMPTY", ""}};
    const auto lookup = MapLookup(vars);

    CHECK(InterpolateEnv("Bearer ${TOKEN}", lookup) == std::optional<std::string>("Bearer s3cret"));
    CHECK(InterpolateEnv("${TOKEN}:${EMPTY}", lookup) == std::optional<std::string>("s3cret:"));
    // Repeated markers, text before/after, and a marker alone.
    CHECK(InterpolateEnv("${TOKEN}-${TOKEN}", lookup) == std::optional<std::string>("s3cret-s3cret"));
    CHECK(InterpolateEnv("plain", lookup) == std::optional<std::string>("plain"));
    // $ without { is literal.
    CHECK(InterpolateEnv("$HOME", lookup) == std::optional<std::string>("$HOME"));
}

TEST_CASE("InterpolateEnv fails closed on unresolvable markers", "[mcp_naming]") {
    const std::map<std::string, std::string> vars = {{"TOKEN", "s3cret"}};
    const auto lookup = MapLookup(vars);

    // Unset variable, empty name, unterminated marker: each must be an error,
    // never a silent empty string or a raw marker sent to a remote server.
    CHECK(InterpolateEnv("${MISSING}", lookup) == std::nullopt);
    CHECK(InterpolateEnv("Bearer ${MISSING}", lookup) == std::nullopt);
    CHECK(InterpolateEnv("${}", lookup) == std::nullopt);
    CHECK(InterpolateEnv("Bearer ${TOKEN", lookup) == std::nullopt);
}

TEST_CASE("InterpolateEnv supports ${NAME:-default}", "[mcp_naming]") {
    const std::map<std::string, std::string> vars = {{"SET", "value"}};
    const auto lookup = MapLookup(vars);

    // Unset -> the fallback text, not a failure naming the whole "NAME:-default"
    // string as a bogus variable.
    CHECK(InterpolateEnv("${MISSING:-fallback}", lookup) == std::optional<std::string>("fallback"));
    // Set -> the real value; the fallback is never consulted.
    CHECK(InterpolateEnv("${SET:-fallback}", lookup) == std::optional<std::string>("value"));
    // An empty default is a valid default, not a typo.
    CHECK(InterpolateEnv("${MISSING:-}", lookup) == std::optional<std::string>(""));
    // A ":-" inside the default itself is not re-split.
    CHECK(InterpolateEnv("${MISSING:-a:-b}", lookup) == std::optional<std::string>("a:-b"));
}

TEST_CASE("InterpolateEnv warns on a bare $VAR without changing its passthrough value",
          "[mcp_naming]") {
    const std::map<std::string, std::string> vars = {{"TOKEN", "s3cret"}};
    const auto lookup = MapLookup(vars);

    std::vector<std::string> warnings;
    auto on_bare_var = [&warnings](std::string_view name) { warnings.emplace_back(name); };

    // The passthrough value is unchanged by the warning -- only a diagnostic
    // is added, never a behavior change.
    CHECK(InterpolateEnv("$HOME", lookup, nullptr, on_bare_var) ==
          std::optional<std::string>("$HOME"));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] == "HOME");

    // A braced marker is not a "bare" reference and must not also warn.
    warnings.clear();
    CHECK(InterpolateEnv("${TOKEN}", lookup, nullptr, on_bare_var) ==
          std::optional<std::string>("s3cret"));
    CHECK(warnings.empty());

    // An incidental '$' not shaped like an identifier (e.g. a price) never warns.
    warnings.clear();
    CHECK(InterpolateEnv("$5.00", lookup, nullptr, on_bare_var) == std::optional<std::string>("$5.00"));
    CHECK(warnings.empty());
}
