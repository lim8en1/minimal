#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include "minimal/client/session_client.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "temp_dir.hpp"

using namespace minimal::client;

namespace {
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

namespace {
using Policy = minimal::plugins::NameTable::Policy;
bool AnyContains(const std::vector<std::string>& messages, const std::string& needle) {
    for (const auto& m : messages) {
        if (m.find(needle) != std::string::npos) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("ResolveClientNames: a duplicate view id is fatal and names both plugins",
          "[tui_bootstrap][names]") {
    // Views and start views share one namespace: a start view colliding with a
    // view is a collision too.
    std::vector<plugins::Attribution> views = {{"a", "files"}};
    std::vector<plugins::Attribution> start_views = {{"b", "files"}};
    const auto names = ResolveClientNames(
        {{"view id", Policy::kName, &views}, {"view id", Policy::kName, &start_views}}, {});
    REQUIRE(names.fatal.size() == 1);
    CHECK(AnyContains(names.fatal, "view id 'files'"));
    CHECK(AnyContains(names.fatal, "'a', 'b'"));
}

TEST_CASE("ResolveClientNames: aliasing one view resolves the collision", "[tui_bootstrap][names]") {
    std::vector<plugins::Attribution> views = {{"a", "files"}, {"b", "files"}};
    const auto names =
        ResolveClientNames({{"view id", Policy::kName, &views}}, {{"b.files", "files2"}});
    CHECK(names.fatal.empty());
    CHECK(names.warnings.empty());
}

TEST_CASE("ResolveClientNames: a slot is never aliased -- the collision stays fatal",
          "[tui_bootstrap][names][invariant]") {
    // Renaming a display renderer's payload kind would only detach it from
    // the payloads it exists to draw; the alias is refused with a warning.
    std::vector<plugins::Attribution> renderers = {{"a", "diff"}, {"b", "diff"}};
    const auto names = ResolveClientNames({{"display renderer", Policy::kSlot, &renderers}},
                                          {{"b.diff", "diff2"}});
    CHECK(AnyContains(names.fatal, "display renderer 'diff'"));
    CHECK(AnyContains(names.warnings, "'b.diff' names a renderer"));
}

TEST_CASE("ResolveClientNames: an alias naming nothing is a warning", "[tui_bootstrap][names]") {
    std::vector<plugins::Attribution> views = {{"fixture", "main"}};
    const auto names = ResolveClientNames({{"view id", Policy::kName, &views}},
                                          {{"fixture.nonexistent", "x"}});
    CHECK(names.fatal.empty());
    CHECK(AnyContains(names.warnings, "'fixture.nonexistent' names nothing"));
}
