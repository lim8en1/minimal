// E5 client extension seams: registry semantics, the loader over a REAL .so
// (dlopen'd, like the server fixtures -- symbol lookup and tag rejection are
// invisible to a mock), discovery round-trips, the attention block, the md4c
// default engine's streaming contract, theme sections, and the ui.json /
// keybindings seams.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "minimal/client/keybindings.hpp"
#include "minimal/client/plugins/client_plugin_loader.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/chat_model.hpp"
#include "temp_dir.hpp"

#ifndef MINIMAL_FIXTURE_DIR
#error "MINIMAL_FIXTURE_DIR must name the built fixture directory"
#endif

using namespace minimal;
using namespace minimal::client;
using namespace minimal::client::plugins;

namespace {

// Renders one element to text so engine/stream output is assertable without
// the "FTXUI snapshot" anti-pattern: this asserts CONTENT a plugin handed us,
// not visual layout.
std::string ToText(const ftxui::Element& element) {
    ftxui::Screen screen(80, 20);
    ftxui::Render(screen, element);
    return screen.ToString();
}

std::string FixturePath(const std::string& name) {
    return (std::filesystem::path(MINIMAL_FIXTURE_DIR) / name).string();
}

struct LoadedFixtures {
    ViewRegistry views;
    ToolResultRendererRegistry renderers;
    MessageRendererRegistry message_renderers;
    StatusSegmentRegistry status_segments;
    SyntaxGrammarRegistry grammars;
    ChatSurfaceRegistry chat_surface;
    MarkdownEngineRegistry markdown_engine;
    AttentionState attention;
    std::vector<PluginRecord> records;

    // NAMED locals, not inline temporaries: a plugin captures the sink/api
    // pointers from its registration context and uses them AFTER LoadAll --
    // a temporary destroyed at the end of the loader-constructor expression
    // left every post-load seam call a use-after-free (the same bug RunTui
    // had; see the attention-seam test below, which exercises post-load use).
    // Null client: the api's documented no-offer behavior.
    std::unique_ptr<AttentionSink> attention_sink;
    std::unique_ptr<ClientApi> client_api;

    // `candidates` loaded against fresh registries.
    static LoadedFixtures Load(const std::vector<PluginCandidate>& candidates) {
        LoadedFixtures f;
        f.attention_sink = MakeHostAttentionSink(&f.attention);
        f.client_api = MakeHostClientApi(nullptr);
        ClientPluginLoader loader(f.views, f.renderers, f.message_renderers, f.status_segments,
                                  f.grammars, f.chat_surface, f.markdown_engine,
                                  f.attention_sink.get(), f.client_api.get());
        f.records = loader.LoadAll(candidates);
        return f;
    }
};

PluginCandidate Candidate(const std::string& file, nlohmann::json config = {}) {
    return PluginCandidate{FixturePath(file), Provenance::Bundled, /*enabled=*/true,
                           std::move(config)};
}

// Minimal in-test implementations for registry-semantics tests.
class StubMessageRenderer final : public MessageRenderer {
public:
    ftxui::Element Render(const ChatRow&, const Theme&) const override {
        return ftxui::text("stub");
    }
};

class StubSurface final : public ChatSurface {
public:
    const Spec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::Renderer([] { return ftxui::text("stub surface"); });
    }
    void OnEvent(const v1::ServerEvent&) override {}
    void OnTheme(const Theme&) override {}

private:
    Spec spec_{"stub", "Stub"};
};

class StubEngine final : public MarkdownEngine {
public:
    ftxui::Element Render(std::string_view, const Theme&) const override {
        return ftxui::text("stub engine");
    }
    std::unique_ptr<Stream> MakeStream() const override { return nullptr; }
};

}  // namespace

TEST_CASE("registries: first claimant wins, duplicates are recorded not applied",
          "[client_plugins]") {
    MessageRendererRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(ChatRowKind::Thinking, std::make_unique<StubMessageRenderer>());
    registry.SetAttribution("plugin-b");
    registry.Register(ChatRowKind::Thinking, std::make_unique<StubMessageRenderer>());

    REQUIRE(registry.claims().size() == 2);
    CHECK(registry.claims()[0].second.plugin_name == "plugin-a");
    CHECK(registry.claims()[1].second.plugin_name == "plugin-b");
    // The active renderer is still the FIRST claimant's: presentation must not
    // depend on plugin load order (the collision rule's whole point).
    CHECK(registry.Find(ChatRowKind::Thinking) != nullptr);
}

TEST_CASE("exclusive registries: second registration warns and is ignored",
          "[client_plugins]") {
    ChatSurfaceRegistry surfaces;
    surfaces.SetAttribution("first");
    surfaces.Register(std::make_unique<StubSurface>());
    REQUIRE(surfaces.record.registered);

    MarkdownEngineRegistry engines;
    engines.SetAttribution("first");
    engines.Register(std::make_unique<StubEngine>());
    CHECK(engines.record.registered);
    engines.SetAttribution("second");
    engines.Register(std::make_unique<StubEngine>());
    CHECK(engines.record.plugin_name == "first");
    // The FIRST engine is still the active one (the second was discarded).
    CHECK(ToText(engines.engine->Render("x", Theme::Parse("{}", 8))) == "stub engine");
}

TEST_CASE("attention state: plugin raise wins until cleared", "[client_plugins]") {
    AttentionState attention;
    CHECK_FALSE(attention.TabRaised("s1"));
    attention.RaiseTab("s1");
    attention.RaiseTab("s2");
    CHECK(attention.TabRaised("s1"));
    CHECK(attention.TabRaised("s2"));
    CHECK(attention.RaisedTabs().size() == 2);
    attention.ClearTab("s1");
    CHECK_FALSE(attention.TabRaised("s1"));
    CHECK(attention.TabRaised("s2"));
    CHECK_FALSE(attention.GlobalRaised());
    attention.RaiseGlobal();
    CHECK(attention.GlobalRaised());
    attention.ClearGlobal();
    CHECK_FALSE(attention.GlobalRaised());
}

TEST_CASE("the client loader loads a real plugin through a real dlopen",
          "[client_plugins][loader]") {
    const auto fixtures =
        LoadedFixtures::Load({Candidate("fixture-client.so", {{"echo_value", "hello"}})});

    REQUIRE(fixtures.records.size() == 1);
    const auto& record = fixtures.records.front();
    REQUIRE(record.ok());
    CHECK(record.name == "fixture-client");
    CHECK(record.abi_tag == std::string(common::AbiTag()));

    // Every additive slot the fixture registered is visible, attributed.
    REQUIRE(fixtures.status_segments.segments().size() == 1);
    // Config echo: the segment renders THIS plugin's own config slice, through
    // a real dlopen.
    CHECK(fixtures.status_segments.segments().front()->Render() == "hello");
    CHECK(fixtures.status_segments.Find("fixture_seg") != nullptr);

    REQUIRE(fixtures.renderers.Find("fixture_tool") != nullptr);

    bool has_minilang = false;
    for (const auto& grammar : fixtures.grammars.grammars()) {
        if (grammar->Language() == "minilang") has_minilang = true;
    }
    CHECK(has_minilang);

    REQUIRE(fixtures.views.Find("fixture.panel") != nullptr);
    REQUIRE(fixtures.views.Find("fixture.modal") != nullptr);
    // The views are live components (panel render is assertable text).
    CHECK(ToText(fixtures.views.Find("fixture.panel")->component()->Render()) ==
          "fixture panel");

    // Message-kind claim recorded (first-claimant semantics tested above).
    bool claimed_thinking = false;
    for (const auto& [kind, attribution] : fixtures.message_renderers.claims()) {
        if (kind == ChatRowKind::Thinking && attribution.plugin_name == "fixture-client") {
            claimed_thinking = true;
        }
    }
    CHECK(claimed_thinking);

    // The exclusive slots were NOT touched by this fixture: the core defaults
    // still stand.
    CHECK_FALSE(fixtures.chat_surface.record.registered);
    CHECK_FALSE(fixtures.markdown_engine.record.registered);
}

TEST_CASE("the exclusive slots ignore a second registration and keep the first",
          "[client_plugins][loader]") {
    // The same .so loaded twice = two plugins claiming the same exclusive
    // slot. The first wins; the second's record says it did NOT land.
    const auto fixtures = LoadedFixtures::Load(
        {Candidate("fixture-client-exclusive.so"), Candidate("fixture-client-exclusive.so")});

    REQUIRE(fixtures.records.size() == 2);
    CHECK(fixtures.records[0].ok());
    CHECK(fixtures.records[1].ok());
    CHECK(fixtures.records[0].registered_chat_surface);
    CHECK_FALSE(fixtures.records[1].registered_chat_surface);
    CHECK(fixtures.records[0].registered_markdown_engine);
    CHECK_FALSE(fixtures.records[1].registered_markdown_engine);
    CHECK(fixtures.chat_surface.record.plugin_name == "fixture-client-exclusive");
    REQUIRE(fixtures.chat_surface.surface != nullptr);

    // The surface receives what the host pushes (the OnEvent contract): a
    // push must be accepted without blocking or crashing; per-surface state
    // observability is the app-state tier's business.
    v1::ServerEvent event;
    event.set_session_id("s-1");
    event.mutable_session_metadata()->set_session_id("s-1");
    fixtures.chat_surface.surface->OnEvent(event);
}

TEST_CASE("the md4c default engine carries the streaming contract", "[client_plugins]") {
    Theme theme = Theme::Parse("{}", 8);
    MarkdownRenderer renderer(&theme, nullptr);
    MarkdownEngineRegistry engines;
    RegisterDefaultMarkdownEngine(engines, renderer);
    REQUIRE(engines.engine != nullptr);
    CHECK(engines.record.registered);
    // Core, not a plugin: the claimant name is cleared.
    CHECK(engines.record.plugin_name.empty());

    // Complete-text render.
    const auto rendered = ToText(engines.engine->Render("hello **world**", theme));
    CHECK(rendered.find("hello") != std::string::npos);
    CHECK(rendered.find("world") != std::string::npos);

    // Streaming: deltas append, Finalize reparses authoritatively. Partial
    // input may be rendered unsoundly by an engine that reparses early -- the
    // contract only fixes the FINALIZED output.
    auto stream = engines.engine->MakeStream();
    stream->AppendDelta("part one, ");
    stream->AppendDelta("part two");
    stream->Finalize();
    const auto finalized = ToText(stream->Render(theme));
    CHECK(finalized.find("part one,") != std::string::npos);
    CHECK(finalized.find("part two") != std::string::npos);
}

TEST_CASE("client plugin discovery: provenance defaults, merge, and round-trip",
          "[client_plugins][discovery]") {
    // Round-trip: unmodelled keys survive the rewrite.
    const std::string original = R"({
        "plugins": [{"file": "todo.so", "provenance": "bundled", "enabled": true,
                     "config": {"tick_ms": 500}}],
        "aliases": {"myplugin.widget": "widget"},
        "future_key": {"anything": true}
    })";
    auto parsed = ParseClientPluginConfig(original);
    REQUIRE(parsed.plugins.size() == 1);
    CHECK(parsed.plugins[0].config["tick_ms"] == 500);
    const auto rewritten = SerializeClientPluginConfig(parsed);
    auto reparsed = ParseClientPluginConfig(rewritten);
    REQUIRE(reparsed.plugins.size() == 1);
    CHECK(reparsed.plugins[0].config["tick_ms"] == 500);
    CHECK(reparsed.aliases.count("myplugin.widget") == 1);
    CHECK(reparsed.preserved.contains("future_key"));

    // Discovery merges new entries in file order: bundled first (default
    // ENABLED), then user (default DISABLED).
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "bundled/one");
    std::filesystem::create_directories(dir.path() / "user");
    std::ofstream(dir.path() / "bundled/one/a.so") << "stub";
    std::ofstream(dir.path() / "bundled/one/b.so") << "stub";
    std::ofstream(dir.path() / "user/c.so") << "stub";

    ClientPluginConfig config;
    const auto candidates =
        DiscoverClientPlugins(dir.path() / "bundled", dir.path() / "user", config);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0].file.filename() == "a.so");
    CHECK(candidates[0].provenance == Provenance::Bundled);
    CHECK(candidates[0].enabled);   // bundled default
    CHECK(candidates[2].file.filename() == "c.so");
    CHECK_FALSE(candidates[2].enabled);   // user default
    REQUIRE(config.plugins.size() == 3);  // written back for persistence
}

TEST_CASE("theme sections merge after core roles and never shadow them",
          "[client_plugins][invariant]") {
    // Section roles are addressable as section.role; a section role colliding
    // with a core role is IGNORED -- core wins, whatever the theme file says.
    const std::string doc = R"({
        "name": "test",
        "colors": {"user_message": "#ff0000"},
        "sections": {"todo_plugin": {"card": "#00ff00", "user_message": "#0000ff"}}
    })";
    const Theme theme = Theme::Parse(doc, 8);
    CHECK(theme.ColorFor("todo_plugin.card") == ftxui::Color(0, 255, 0));
    // Core wins on the collision attempt:
    CHECK(theme.ColorFor("user_message") == ftxui::Color(255, 0, 0));
}

TEST_CASE("ui.json: missing and malformed files degrade to defaults", "[client_plugins]") {
    TempDir dir;
    auto ui = LoadUiConfig(dir.path());
    CHECK(ui.status_format.empty());

    std::ofstream(dir.path() / "ui.json") << "{ not json";
    ui = LoadUiConfig(dir.path());
    CHECK(ui.status_format.empty());

    std::ofstream(dir.path() / "ui.json") << R"({"status_format": "{a} | {b}", "future": 1})";
    ui = LoadUiConfig(dir.path());
    CHECK(ui.status_format == "{a} | {b}");
}

TEST_CASE("keybindings EnsureDefault never overwrites an existing binding",
          "[client_plugins]") {
    auto keys = Keybindings::Parse(R"({"fixture.panel": "ctrl+g"})");
    CHECK_FALSE(keys.EnsureDefault("fixture.panel", "ctrl+p"));
    CHECK(keys.KeyFor("fixture.panel") == "ctrl+g");
    CHECK(keys.EnsureDefault("fixture.other", "ctrl+u"));
    CHECK(keys.KeyFor("fixture.other") == "ctrl+u");
}

TEST_CASE("command notices carry the display mode and are type-disjoint from chat rows",
          "[client_plugins][invariant]") {
    // The §4.4 invariant, client half: a CommandResult becomes a CommandNotice
    // -- display fields intact -- and CommandNotice has no path into the chat
    // row model. A View result with a view_hint survives the trip; the TUI
    // opens the view instead of rendering a line.
    v1::CommandResult result;
    result.set_request_id("cmd-1");
    result.set_status(v1::COMMAND_STATUS_OK);
    result.set_display(v1::COMMAND_DISPLAY_VIEW);
    result.set_view_hint("sessions");
    const auto notice = NoticeFromCommandResult(result);
    CHECK(notice.display == v1::COMMAND_DISPLAY_VIEW);
    CHECK(notice.view_hint == "sessions");
    CHECK_FALSE(notice.is_error);

    // An error result rides the STATUS display (dispatcher downgrade).
    v1::CommandResult failed;
    failed.set_status(v1::COMMAND_STATUS_ERROR);
    const auto error_notice = NoticeFromCommandResult(failed);
    CHECK(error_notice.is_error);
    CHECK(error_notice.display == v1::COMMAND_DISPLAY_STATUS);
}

// Track R1b-1's load-bearing test: the host's seam objects (AttentionSink,
// ClientApi) were bound INLINE into the loader constructor -- unique_ptr
// temporaries destroyed at the end of that full expression, while every
// plugin's registration context kept the raw pointers for post-load use.
// The fixture's status segment raises tab attention from Render(), which is
// post-load seam use exactly like a real plugin's; with the temporaries
// restored this test is an ASan finding and the raise never lands.
TEST_CASE("host seams survive the load: a plugin raises attention AFTER LoadAll",
          "[client_plugins][seams]") {
    const auto fixtures = LoadedFixtures::Load({Candidate(
        "fixture-client.so", {{"echo_value", "seam-check"}, {"raise_tab", "seam-session"}})});
    REQUIRE(fixtures.records.size() == 1);
    REQUIRE(fixtures.records[0].ok());

    const auto* segment = fixtures.status_segments.Find("fixture_seg");
    REQUIRE(segment != nullptr);

    // Post-load use: the load is long over; the sink must still be alive.
    CHECK(segment->Render() == "seam-check");
    CHECK(fixtures.attention.TabRaised("seam-session"));
}
