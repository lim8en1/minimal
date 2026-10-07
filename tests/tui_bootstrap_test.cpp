#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <ftxui/component/component.hpp>

#include "minimal/client/session_client.hpp"
#include "minimal/client/plugins/client_plugin_loader.hpp"
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
#ifndef MINIMAL_UI_KIT_PLUGIN
#error "MINIMAL_UI_KIT_PLUGIN must name the built ui-kit plugin"
#endif
#ifndef MINIMAL_PROMPT_EDITOR_PLUGIN
#error "MINIMAL_PROMPT_EDITOR_PLUGIN must name the built prompt-editor plugin"
#endif
#ifndef MINIMAL_CONSENT_PROMPTS_PLUGIN
#error "MINIMAL_CONSENT_PROMPTS_PLUGIN must name the built consent-prompts plugin"
#endif

namespace {

// The widget-kit slot and core's prompt kinds are required, and from the
// test binary the bundled plugin dir is not the build's (it probes relative
// to the executable): the built plugins go in as user plugins, enabled.
void SeedPlugins(const std::filesystem::path& config_dir,
                 const std::vector<std::filesystem::path>& plugins) {
    std::filesystem::create_directories(config_dir / "client-plugins");
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& plugin : plugins) {
        std::filesystem::copy_file(plugin, config_dir / "client-plugins" / plugin.filename());
        entries.push_back(
            {{"file", plugin.filename().string()}, {"provenance", "user"}, {"enabled", true}});
    }
    std::ofstream(config_dir / "client-plugins.json") << nlohmann::json{{"plugins", entries}}.dump();
}

void SeedRequiredPlugins(const std::filesystem::path& config_dir) {
    SeedPlugins(config_dir,
                {MINIMAL_UI_KIT_PLUGIN, MINIMAL_CONSENT_PROMPTS_PLUGIN, MINIMAL_PROMPT_EDITOR_PLUGIN});
}

}  // namespace

// The client side of the server's plugin_matrix: with no client plugin at
// all the TUI refuses to start -- nothing fills the required widget-kit slot
// -- with any one required plugin alone it refuses, with all of them it starts.
TEST_CASE("the TUI starts only with the widget kit, the consent prompts and an editor",
          "[tui_bootstrap][widget_kit][prompts]") {
    SessionClient client("unix:///nonexistent/minimal-tui-bootstrap-test.sock");
    {
        minimal::testing::TempDir config_dir;
        ScopedEnv config_override("MINIMAL_CONFIG_DIR", config_dir.path().c_str());
        TuiEnvironment env;
        CHECK_FALSE(BuildTuiEnvironment(client, env));
    }
    for (const char* only :
         {MINIMAL_UI_KIT_PLUGIN, MINIMAL_CONSENT_PROMPTS_PLUGIN, MINIMAL_PROMPT_EDITOR_PLUGIN}) {
        INFO(only);
        minimal::testing::TempDir config_dir;
        SeedPlugins(config_dir.path(), {only});
        ScopedEnv config_override("MINIMAL_CONFIG_DIR", config_dir.path().c_str());
        TuiEnvironment env;
        CHECK_FALSE(BuildTuiEnvironment(client, env));
    }
    {
        minimal::testing::TempDir config_dir;
        SeedRequiredPlugins(config_dir.path());
        ScopedEnv config_override("MINIMAL_CONFIG_DIR", config_dir.path().c_str());
        TuiEnvironment env;
        REQUIRE(BuildTuiEnvironment(client, env));
        CHECK(env.registries.widget_kit.holder() == "ui-kit");
        CHECK(env.registries.prompt_renderers.Find("permission") != nullptr);
        CHECK(env.registries.prompt_editor.holder() == "prompt-editor");
        // Its actions are bindable: the defaults it declared are in the keymap.
        CHECK(env.keybindings.KeyFor("prompt.newline") == "alt+enter");
    }
}

TEST_CASE("BuildTuiEnvironment's highlighter outlives the call and highlights code",
          "[tui_bootstrap]") {
    minimal::testing::TempDir config_dir;
    SeedRequiredPlugins(config_dir.path());
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

class StubEngine final : public plugins::MarkdownEngine {
public:
    ftxui::Element Render(std::string_view, const Theme&) const override {
        return ftxui::text("stub engine");
    }
    std::unique_ptr<Stream> MakeStream() const override { return nullptr; }
};

class StubSurface final : public plugins::ChatSurface {
public:
    const Spec& spec() const override { return spec_; }
    ftxui::Component component(const std::string&) override {
        return ftxui::Renderer([] { return ftxui::text("stub surface"); });
    }
    void OnEvent(const minimal::v1::ServerEvent&) override {}
    void OnTheme(const Theme&) override {}

private:
    Spec spec_{"stub", "Stub"};
};

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

TEST_CASE("two plugins claiming one exclusive slot is fatal at startup, naming both",
          "[tui_bootstrap][names][regression]") {
    // The bug: first registration won with a warning, so which chat view or
    // markdown engine ran depended on load order -- and bundled plugins load
    // first, so a bundled default could never be replaced by enabling another.
    plugins::ClientRegistries registries;
    registries.markdown_engine.SetAttribution("bundled-md");
    registries.markdown_engine.Register(std::make_unique<StubEngine>());
    registries.markdown_engine.SetAttribution("user-md");
    registries.markdown_engine.Register(std::make_unique<StubEngine>());
    registries.markdown_engine.ClearAttribution();

    std::vector<NameFeed> feeds;
    for (const auto& hook : plugins::RegistryHooks(registries)) {
        if (hook.name_space.empty()) continue;
        feeds.push_back({hook.name_space, hook.policy, &hook.registry->attributions()});
    }
    const auto names = ResolveClientNames(feeds, {});
    REQUIRE(names.fatal.size() == 1);
    CHECK(AnyContains(names.fatal, "exclusive slot 'markdown_engine'"));
    CHECK(AnyContains(names.fatal, "'bundled-md', 'user-md'"));
    CHECK(AnyContains(names.fatal, "disable one of the plugins"));

    // One claimant -- and the core default filling an empty slot -- is fine.
    plugins::ClientRegistries single;
    single.chat_surface.SetAttribution("my-chat");
    single.chat_surface.Register(std::make_unique<StubSurface>());
    single.chat_surface.ClearAttribution();
    std::vector<NameFeed> single_feeds;
    for (const auto& hook : plugins::RegistryHooks(single)) {
        if (hook.name_space.empty()) continue;
        single_feeds.push_back({hook.name_space, hook.policy, &hook.registry->attributions()});
    }
    CHECK(ResolveClientNames(single_feeds, {}).fatal.empty());
}

TEST_CASE("ResolveClientNames: an alias naming nothing is a warning", "[tui_bootstrap][names]") {
    std::vector<plugins::Attribution> views = {{"fixture", "main"}};
    const auto names = ResolveClientNames({{"view id", Policy::kName, &views}},
                                          {{"fixture.nonexistent", "x"}});
    CHECK(names.fatal.empty());
    CHECK(AnyContains(names.warnings, "'fixture.nonexistent' names nothing"));
}
