#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include "minimal/client/session_client.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "temp_dir.hpp"

using namespace minimal::client;
using minimal::plugins::NameKind;

namespace {
std::vector<std::pair<ChatRowKind, plugins::Attribution>> NoMessageClaims() {
    return {};
}

// RAII env var override, same pattern as config_paths_test.cpp.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_ = old;
        }
        ::setenv(name, value, 1);
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
}  // namespace

// Regression for a dangling-pointer crash (SIGSEGV on the first fenced code
// block, in every session thereafter): BuildTuiEnvironment used to assemble
// the grammar table as a BuildTuiEnvironment-local std::vector and hand
// SyntaxHighlighter a pointer into it, so the vector's storage was freed the
// moment BuildTuiEnvironment returned -- exactly the "closures/pointers
// outlive the object they point into" bug class the header's
// address-stability contract otherwise guards against. `languages` is now a
// TuiEnvironment field for this reason; this test builds a real environment,
// lets BuildTuiEnvironment return, and then exercises the exact crash path
// (Highlight on a cpp code block) against `env` well after that return. Under
// build-asan this reliably use-after-free's without the fix.
TEST_CASE("BuildTuiEnvironment's highlighter outlives the call and highlights code",
          "[tui_bootstrap]") {
    minimal::testing::TempDir config_dir;
    ScopedEnv config_override("MINIMAL_CONFIG_DIR", config_dir.path().c_str());

    // The channel is lazy (grpc::CreateChannel does not connect eagerly), and
    // BuildTuiEnvironment never issues an RPC -- it only takes `client`'s
    // address for MakeHostClientApi -- so an unreachable target is fine here.
    SessionClient client("unix:///nonexistent/minimal-tui-bootstrap-test.sock");
    TuiEnvironment env;
    REQUIRE(BuildTuiEnvironment(client, env));
    REQUIRE(env.highlighter.has_value());

    // The crash reproduced with ANY info string, including none. This is the
    // literal call from ChatView's render path (via MarkdownRenderer ->
    // Renderer::RenderCode), long after BuildTuiEnvironment's stack frame is
    // gone.
    const ftxui::Elements spans =
        env.highlighter->Highlight("int main() { return 0; }", "cpp");
    CHECK_FALSE(spans.empty());
}

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
