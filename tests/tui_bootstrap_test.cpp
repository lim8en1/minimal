#include <catch2/catch_test_macros.hpp>

#include "minimal/client/tui_bootstrap.hpp"

using namespace minimal::client;
using minimal::plugins::NameKind;

namespace {
std::vector<std::pair<ChatRowKind, plugins::Attribution>> NoMessageClaims() {
    return {};
}
}  // namespace

TEST_CASE("MatchAliasKind matches a view_registry attribution", "[tui_bootstrap]") {
    std::vector<plugins::Attribution> views = {{"fixture", "main"}};
    std::vector<AliasCandidate> candidates = {
        {NameKind::ViewId, &views},
    };
    CHECK(MatchAliasKind(candidates, NoMessageClaims(), "fixture.main") == NameKind::ViewId);
}

TEST_CASE("MatchAliasKind matches a start_view_registry-only attribution as ViewId",
          "[tui_bootstrap]") {
    // Regression: alias_on_kind used to only check view_registry, so a
    // plugin's *start view* alias (registered only in start_view_registry,
    // never view_registry) fell through to the dangling ViewId fallback --
    // which happened to also be ViewId, silently masking the bug. Assert the
    // match comes from the start_view_registry candidate itself, not the
    // fallback, by also asserting a genuinely unmatched key stays dangling.
    std::vector<plugins::Attribution> views;
    std::vector<plugins::Attribution> start_views = {{"fixture", "start"}};
    std::vector<AliasCandidate> candidates = {
        {NameKind::ViewId, &views},
        {NameKind::ViewId, &start_views},
    };
    CHECK(MatchAliasKind(candidates, NoMessageClaims(), "fixture.start") == NameKind::ViewId);
}

TEST_CASE("MatchAliasKind matches a message_registry claim", "[tui_bootstrap]") {
    std::vector<AliasCandidate> candidates;
    std::vector<std::pair<ChatRowKind, plugins::Attribution>> claims = {
        {ChatRowKind::AgentMarkdown, {"fixture", "markdown"}},
    };
    CHECK(MatchAliasKind(candidates, claims, "fixture.markdown") == NameKind::MessageRenderer);
}

TEST_CASE("MatchAliasKind falls back to ViewId for an unmatched key", "[tui_bootstrap]") {
    std::vector<plugins::Attribution> views = {{"fixture", "main"}};
    std::vector<AliasCandidate> candidates = {
        {NameKind::ViewId, &views},
    };
    CHECK(MatchAliasKind(candidates, NoMessageClaims(), "fixture.nonexistent") == NameKind::ViewId);
}
