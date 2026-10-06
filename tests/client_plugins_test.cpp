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
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "minimal/client/keybindings.hpp"
#include "minimal/client/plugins/client_plugin_loader.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/client/chat_model.hpp"
#include "temp_dir.hpp"

#ifndef MINIMAL_FIXTURE_DIR
#error "MINIMAL_FIXTURE_DIR must name the built fixture directory"
#endif

using namespace minimal;
using namespace minimal::client;
using namespace minimal::client::plugins;
using minimal::testing::TempDir;

namespace {

// Renders one element to text so engine/stream output is assertable without
// the "FTXUI snapshot" anti-pattern: this asserts CONTENT a plugin handed us,
// not visual layout.
std::string ToText(const ftxui::Element& element) {
    ftxui::Screen screen(80, 20);
    ftxui::Render(screen, element);
    std::string out = screen.ToString();
    // Screen::ToString separates rows with \r\n, so both halves get trimmed.
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

std::string FixturePath(const std::string& name) {
    return (std::filesystem::path(MINIMAL_FIXTURE_DIR) / name).string();
}

struct LoadedFixtures {
    ClientRegistries registries;
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
    std::unique_ptr<FocusProvider> focus_provider;

    // `candidates` loaded against fresh registries. unique_ptr, not a value:
    // AttentionState holds a mutex, so the struct cannot move, and NRVO still
    // requires a movable return type.
    static std::unique_ptr<LoadedFixtures> Load(const std::vector<PluginCandidate>& candidates) {
        auto f = std::make_unique<LoadedFixtures>();
        f->attention_sink = MakeHostAttentionSink(&f->attention);
        f->client_api = MakeHostClientApi(nullptr);
        f->focus_provider = MakeHostFocusProvider();
        ClientPluginLoader loader(f->registries, {.attention = f->attention_sink.get(),
                                                  .client_api = f->client_api.get(),
                                                  .focus = f->focus_provider.get()});
        f->records = loader.LoadAll(candidates);
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
    ftxui::Element Render(const ChatRow&, const Theme&,
                          const RowRenderContext&) const override {
        return ftxui::text("stub");
    }
};

class StubSurface final : public ChatSurface {
public:
    const Spec& spec() const override { return spec_; }
    ftxui::Component component(const std::string&) override {
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

class StubStartView final : public StartView {
public:
    explicit StubStartView(std::string id) : id_(std::move(id)) {}
    const ViewSpec& spec() const override { return spec_; }
    ftxui::Element Render(const std::string& session_id, const Theme&) const override {
        return ftxui::text("stub start:" +
                           (session_id.empty() ? std::string("(empty)") : session_id));
    }

private:
    std::string id_;
    ViewSpec spec_{id_, "Stub Start"};
};

}  // namespace

TEST_CASE("registries: first claimant wins, duplicates are recorded not applied",
          "[client_plugins]") {
    MessageRendererRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(ChatRowKind::Thinking, std::make_unique<StubMessageRenderer>());
    registry.SetAttribution("plugin-b");
    registry.Register(ChatRowKind::Thinking, std::make_unique<StubMessageRenderer>());

    REQUIRE(registry.attributions().size() == 2);
    CHECK(registry.attributions()[0].plugin_name == "plugin-a");
    CHECK(registry.attributions()[1].plugin_name == "plugin-b");
    CHECK(registry.PluginClaimed(ChatRowKind::Thinking));
    // The active renderer is still the FIRST claimant's: presentation must not
    // depend on plugin load order (the collision rule's whole point).
    CHECK(registry.Find(ChatRowKind::Thinking) != nullptr);
}

TEST_CASE("start view registry: additive, attributed, id-addressable", "[client_plugins]") {
    StartViewRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(std::make_unique<StubStartView>("stub.start"));
    registry.SetAttribution("plugin-b");
    registry.Register(std::make_unique<StubStartView>("stub.start.two"));

    // Additive: BOTH render (registration order), unlike the exclusive slots.
    REQUIRE(registry.All().size() == 2);
    REQUIRE(registry.attributions().size() == 2);
    CHECK(registry.attributions()[0].plugin_name == "plugin-a");
    CHECK(registry.attributions()[0].original_name == "stub.start");
    CHECK(registry.attributions()[1].plugin_name == "plugin-b");
    CHECK(registry.attributions()[1].original_name == "stub.start.two");
    CHECK(registry.Find("stub.start") != nullptr);
    CHECK(registry.Find("stub.start.two") != nullptr);
    CHECK(registry.Find("nothing") == nullptr);

    // The per-render session context travels with the call -- the mechanism a
    // per-session random banner keys on.
    const auto rendered = ToText(registry.Find("stub.start")->Render("sess-9",
                                                                    Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor)));
    CHECK(rendered.find("sess-9") != std::string::npos);
    CHECK(ToText(registry.Find("stub.start")->Render("", Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor))) ==
          "stub start:(empty)");
}

TEST_CASE("AdditiveRegistry: an alias renames the surface Find answers to",
          "[client_plugins][registries][names]") {
    StartViewRegistry registry;
    registry.SetAttribution("a");
    registry.Register(std::make_unique<StubStartView>("files"));
    registry.SetAttribution("b");
    registry.Register(std::make_unique<StubStartView>("files"));
    registry.Register(std::make_unique<StubStartView>("other"));
    registry.ClearAttribution();

    registry.ApplyAliases({{"b.files", "files2"}, {"nobody.x", "y"}});
    CHECK(registry.surfaces() == std::vector<std::string>{"files", "files2", "other"});
    // Both colliding views are reachable, each under its own surface.
    CHECK(registry.Find("files") == registry.All()[0].get());
    CHECK(registry.Find("files2") == registry.All()[1].get());
    // The canonical id is unchanged: the plugin's own code still names "files".
    CHECK(registry.All()[1]->spec().id == "files");

    // Rollback keeps surfaces aligned with the entries that remain.
    registry.RemoveByAttribution("a");
    CHECK(registry.surfaces() == std::vector<std::string>{"files2", "other"});
    CHECK(registry.Find("files") == nullptr);
    CHECK(registry.Find("other") == registry.All()[1].get());
}

TEST_CASE("a plugin's bound HostUi stamps its name on view requests",
          "[client_plugins][names]") {
    auto provider = minimal::client::plugins::MakeHostFocusProvider();
    HostUi* bound = provider->UiFor("b");
    CHECK(provider->UiFor("b") == bound);  // one per plugin, stable
    bound->ShowView("files");
    provider->ShowView("files2");          // the shared seam: a surface request
    const auto requests = provider->TakeViewRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].plugin == "b");
    CHECK(requests[0].view_id == "files");
    CHECK(requests[1].plugin.empty());
}

TEST_CASE("AdditiveRegistry: attribution, first-wins Find, rollback, seal",
          "[client_plugins][registries]") {
    StartViewRegistry registry;
    registry.Register(std::make_unique<StubStartView>("core.start"));   // no window: core
    registry.SetAttribution("a");
    registry.Register(std::make_unique<StubStartView>("dup"));
    registry.Register(nullptr);                                         // ignored
    registry.Register(std::make_unique<StubStartView>(""));             // empty key: ignored
    registry.SetAttribution("b");
    registry.Register(std::make_unique<StubStartView>("dup"));
    registry.Register(std::make_unique<StubStartView>("b.only"));
    registry.ClearAttribution();

    REQUIRE(registry.attributions().size() == 4);
    CHECK(registry.attributions()[0].plugin_name.empty());
    CHECK(registry.attributions()[1].plugin_name == "a");
    CHECK(registry.attributions()[2].plugin_name == "b");
    // A duplicate key is kept (the collision scan's input); Find answers
    // with the first registration.
    CHECK(registry.Find("dup") == registry.All()[1].get());

    // Rollback strips only b's entries, keeping the rest index-aligned.
    registry.RemoveByAttribution("b");
    REQUIRE(registry.All().size() == 2);
    REQUIRE(registry.attributions().size() == 2);
    CHECK(registry.Find("b.only") == nullptr);
    CHECK(registry.All()[1]->spec().id == "dup");
    CHECK(registry.attributions()[1].plugin_name == "a");

    registry.Seal();
    registry.SetAttribution("late");
    registry.Register(std::make_unique<StubStartView>("late.start"));
    CHECK(registry.Find("late.start") == nullptr);
    CHECK(registry.attributions().size() == 2);
}

TEST_CASE("ExclusiveSlot: holder only, rollback releases only the holder, seal",
          "[client_plugins][registries]") {
    MarkdownEngineRegistry slot;
    CHECK_FALSE(slot.held());
    slot.SetAttribution("first");
    slot.Register(std::make_unique<StubEngine>());
    slot.SetAttribution("second");
    slot.Register(std::make_unique<StubEngine>());   // warns, ignored
    REQUIRE(slot.held());
    CHECK(slot.holder() == "first");
    REQUIRE(slot.attributions().size() == 1);
    CHECK(slot.attributions()[0].original_name == "markdown_engine");

    slot.RemoveByAttribution("second");   // never held it: no-op
    CHECK(slot.held());
    slot.RemoveByAttribution("first");
    CHECK_FALSE(slot.held());
    CHECK(slot.get() == nullptr);
    CHECK(slot.attributions().empty());

    slot.Seal();
    slot.SetAttribution("late");
    slot.Register(std::make_unique<StubEngine>());
    CHECK_FALSE(slot.held());
}

TEST_CASE("exclusive registries: second registration warns and is ignored",
          "[client_plugins]") {
    ChatSurfaceRegistry surfaces;
    surfaces.SetAttribution("first");
    surfaces.Register(std::make_unique<StubSurface>());
    REQUIRE(surfaces.held());

    MarkdownEngineRegistry engines;
    engines.SetAttribution("first");
    engines.Register(std::make_unique<StubEngine>());
    CHECK(engines.held());
    engines.SetAttribution("second");
    engines.Register(std::make_unique<StubEngine>());
    CHECK(engines.holder() == "first");
    // Only the holder is recorded.
    CHECK(engines.attributions().size() == 1);
    // The FIRST engine is still the active one (the second was discarded).
    CHECK(ToText(engines.get()->Render("x", Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor))) == "stub engine");
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
}

TEST_CASE("the client loader verifies the ABI tag before any plugin code runs",
          "[client_plugins][loader][invariant]") {
    // fixture-client-bad-abi's name()/version() ABORT if the loader ever
    // calls them: the bug this pins (the client loader ran both before the
    // tag check, which the server loader had already fixed) is exactly
    // "plugin code executed against an incompatible ABI". A clean
    // AbiMismatch record here therefore proves the gate order; running the
    // old order dies in the fixture instead.
    const auto fixtures = LoadedFixtures::Load({Candidate("fixture-client-bad-abi.so")});

    REQUIRE(fixtures->records.size() == 1);
    const auto& record = fixtures->records.front();
    CHECK(record.status == LoadStatus::AbiMismatch);
    CHECK(record.abi_tag == "definitely-not-this-toolchain");
    CHECK(record.name.empty());  // never read
}

TEST_CASE("the client loader loads a real plugin through a real dlopen",
          "[client_plugins][loader]") {
    const auto fixtures =
        LoadedFixtures::Load({Candidate("fixture-client.so", {{"echo_value", "hello"}})});
    REQUIRE(fixtures->records.size() == 1);
    const auto& record = fixtures->records.front();
    REQUIRE(record.ok());
    CHECK(record.name == "fixture-client");
    CHECK(record.abi_tag == std::string(minimal::common::AbiTag()));

    // Every additive slot the fixture registered is visible, attributed.
    REQUIRE(fixtures->registries.status_segments.All().size() == 1);
    // Config echo: the segment renders THIS plugin's own config slice, through
    // a real dlopen.
    CHECK(fixtures->registries.status_segments.All().front()->Render("") == "hello");
    CHECK(fixtures->registries.status_segments.Find("fixture_seg") != nullptr);

    REQUIRE(fixtures->registries.tool_renderers.Find("fixture_tool") != nullptr);
    REQUIRE(fixtures->registries.display_renderers.Find("fixture_kind") != nullptr);

    // A client-side /-command, attributed and runnable.
    REQUIRE(fixtures->registries.client_commands.All().size() == 1);
    CHECK(fixtures->registries.client_commands.All().front()->name() == "fixture-cmd");
    CHECK(record.registered.at("client_commands") == std::vector<std::string>{"fixture-cmd"});
    fixtures->registries.client_commands.All().front()->Run({"a"}, "a b");
    CHECK(record.registered.at("display_renderers") == std::vector<std::string>{"fixture_kind"});

    bool has_minilang = false;
    for (const auto& grammar : fixtures->registries.grammars.All()) {
        if (grammar->Language() == "minilang") has_minilang = true;
    }
    CHECK(has_minilang);

    REQUIRE(fixtures->registries.views.Find("fixture.panel") != nullptr);
    REQUIRE(fixtures->registries.views.Find("fixture.modal") != nullptr);
    // The views are live components (panel render is assertable text).
    CHECK(ToText(fixtures->registries.views.Find("fixture.panel")->component()->Render()) ==
          "fixture panel");

    // The start view landed through a real dlopen too, and the per-render
    // session context reaches it (empty string = the app-level empty state).
    REQUIRE(fixtures->records.front().registered.at("start_views").size() == 1);
    REQUIRE(fixtures->registries.start_views.Find("fixture.start") != nullptr);
    CHECK(ToText(fixtures->registries.start_views.Find("fixture.start")->Render("", Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor))) ==
          "fixture start:");
    CHECK(ToText(fixtures->registries.start_views.Find("fixture.start")->Render("sess-1",
                                                                    Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor))) ==
          "fixture start:sess-1");

    // Message-kind claim recorded (first-claimant semantics tested above).
    bool claimed_thinking = false;
    for (const auto& attribution : fixtures->registries.message_renderers.attributions()) {
        if (attribution.original_name == ToString(ChatRowKind::Thinking) &&
            attribution.plugin_name == "fixture-client") {
            claimed_thinking = true;
        }
    }
    CHECK(claimed_thinking);

    // The exclusive slots were NOT touched by this fixture: the core defaults
    // still stand.
    CHECK_FALSE(fixtures->registries.chat_surface.held());
    CHECK_FALSE(fixtures->registries.markdown_engine.held());
}

TEST_CASE("a second plugin with a loaded plugin's name is refused before any hook runs",
          "[client_plugins][loader]") {
    // The same .so loaded twice = the same plugin NAME twice. Attribution and
    // rollback are keyed by name, so the second copy is refused outright; the
    // first keeps its exclusive slots. (Two DIFFERENT plugins claiming one
    // exclusive slot is the registry-level test above.)
    const auto fixtures = LoadedFixtures::Load(
        {Candidate("fixture-client-exclusive.so"), Candidate("fixture-client-exclusive.so")});

    REQUIRE(fixtures->records.size() == 2);
    CHECK(fixtures->records[0].ok());
    CHECK(fixtures->records[1].status == LoadStatus::DuplicateName);
    CHECK(fixtures->registries.chat_surface.held());
    CHECK(fixtures->registries.chat_surface.holder() == "fixture-client-exclusive");
    CHECK(fixtures->registries.markdown_engine.held());
    CHECK(fixtures->registries.markdown_engine.holder() == "fixture-client-exclusive");
    REQUIRE(fixtures->registries.chat_surface.get() != nullptr);

    // The surface receives what the host pushes (the OnEvent contract): a
    // push must be accepted without blocking or crashing; per-surface state
    // observability is the app-state tier's business.
    v1::ServerEvent event;
    event.set_session_id("s-1");
    event.mutable_session_metadata()->set_session_id("s-1");
    fixtures->registries.chat_surface.get()->OnEvent(event);
}

TEST_CASE("a partial registration is rolled back, and a later plugin reusing "
          "the same view id loads clean",
          "[client_plugins][loader]") {
    // fixture-client-partial-throw: register_views succeeds (claims
    // "fixture.panel"), register_renderers throws. Without rollback, the
    // view would stay live and the SECOND candidate below (fixture-client,
    // which also claims "fixture.panel") would collide against a plugin that
    // never actually loaded.
    const auto fixtures = LoadedFixtures::Load(
        {Candidate("fixture-client-partial-throw.so"), Candidate("fixture-client.so")});

    REQUIRE(fixtures->records.size() == 2);
    CHECK_FALSE(fixtures->records[0].ok());
    CHECK(fixtures->records[0].name == "fixture-client-partial-throw");
    CHECK(fixtures->records[1].ok());
    CHECK(fixtures->records[1].name == "fixture-client");

    // Exactly ONE "fixture.panel" attribution survives, and it belongs to
    // the plugin that actually loaded -- the failed plugin's own claim was
    // stripped, not left to collide.
    int panel_attributions = 0;
    std::string owner;
    for (const auto& attribution : fixtures->registries.views.attributions()) {
        if (attribution.original_name == "fixture.panel") {
            ++panel_attributions;
            owner = attribution.plugin_name;
        }
    }
    CHECK(panel_attributions == 1);
    CHECK(owner == "fixture-client");

    // Find() resolves to the healthy plugin's component, not a dangling
    // pointer into whatever the failed plugin registered.
    REQUIRE(fixtures->registries.views.Find("fixture.panel") != nullptr);
    CHECK(ToText(fixtures->registries.views.Find("fixture.panel")->component()->Render()) ==
          "fixture panel");
}

TEST_CASE("the md4c default engine carries the streaming contract", "[client_plugins]") {
    Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    MarkdownRenderer renderer(&theme, nullptr);
    MarkdownEngineRegistry engines;
    RegisterDefaultMarkdownEngine(engines, renderer);
    REQUIRE(engines.get() != nullptr);
    CHECK(engines.held());
    // Core, not a plugin: the claimant name is cleared.
    CHECK(engines.holder().empty());

    // Complete-text render.
    const auto rendered = ToText(engines.get()->Render("hello **world**", theme));
    CHECK(rendered.find("hello") != std::string::npos);
    CHECK(rendered.find("world") != std::string::npos);

    // Streaming: deltas append, Finalize reparses authoritatively. Partial
    // input may be rendered unsoundly by an engine that reparses early -- the
    // contract only fixes the FINALIZED output.
    auto stream = engines.get()->MakeStream();
    stream->AppendDelta("part one, ");
    stream->AppendDelta("part two");
    stream->Finalize();
    const auto finalized = ToText(stream->Render(theme));
    CHECK(finalized.find("part one,") != std::string::npos);
    CHECK(finalized.find("part two") != std::string::npos);
}

TEST_CASE("the md4c default engine renders GFM table HEADER cells", "[client_plugins]") {
    // md4c emits the distinct MD_BLOCK_TH enum for a header cell, which the
    // renderer's cell handling used to key on MD_BLOCK_TD alone -- header
    // cells fell to the empty-text default and every GFM table lost its
    // header row.
    Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    MarkdownRenderer renderer(&theme, nullptr);
    const auto rendered =
        ToText(renderer.Render("| Head | Tail |\n| --- | --- |\n| 1 | 2 |\n"));
    CHECK(rendered.find("Head") != std::string::npos);
    CHECK(rendered.find("Tail") != std::string::npos);
    CHECK(rendered.find("1") != std::string::npos);   // body cells still render
}

TEST_CASE("client plugin discovery: provenance defaults, merge, and round-trip",
          "[client_plugins][discovery]") {
    // Round-trip: unmodelled keys survive the rewrite.
    const std::string original = R"({
        "plugins": [{"file": "todo.so", "provenance": "bundled", "enabled": true}],
        "aliases": {"myplugin.widget": "widget"},
        "future_key": {"anything": true}
    })";
    auto parsed = ParseClientPluginConfig(original);
    REQUIRE(parsed.plugins.size() == 1);
    const auto rewritten = SerializeClientPluginConfig(parsed);
    auto reparsed = ParseClientPluginConfig(rewritten);
    REQUIRE(reparsed.plugins.size() == 1);
    CHECK(reparsed.plugins[0].enabled == true);
    CHECK(reparsed.aliases.count("myplugin.widget") == 1);

    // An inline config is refused, naming the client's own per-plugin file.
    const auto inline_config = ParseClientPluginConfig(
        R"({"plugins": [{"file": "todo.so", "config": {"tick_ms": 500}}]})");
    REQUIRE(inline_config.problems.size() == 1);
    CHECK(inline_config.problems[0].find("client-plugins.d/todo.json") != std::string::npos);
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

TEST_CASE("client plugin config: a wrong-typed entry is a named problem, never a default",
          "[client_plugins][discovery]") {
    // A typo'd entry used to degrade to "disabled" / "user" silently. Now each
    // bad entry is dropped AND named, the host warns, and the file is never
    // rewritten over the user's bytes.
    const std::string text = R"({
        "plugins": [
            {"file": "a.so", "enabled": "false"},
            {"file": "c.so", "provenance": 5, "enabled": true},
            {"file": "d.so", "enable": true},
            {"file": "ok.so", "enabled": true}
        ]
    })";

    ClientPluginConfig parsed;
    REQUIRE_NOTHROW(parsed = ParseClientPluginConfig(text));
    REQUIRE(parsed.plugins.size() == 1);
    CHECK(parsed.plugins[0].file == "ok.so");
    CHECK(parsed.plugins[0].enabled == true);
    CHECK_FALSE(parsed.plugins[0].provenance.has_value());   // resolved at discovery

    REQUIRE(parsed.problems.size() == 3);
    CHECK(parsed.problems[0].find("plugins[0].enabled: must be true or false") != std::string::npos);
    CHECK(parsed.problems[1].find("plugins[1].provenance: must be a string") != std::string::npos);
    CHECK(parsed.problems[2].find("plugins[2].enable: unknown field") != std::string::npos);
}

TEST_CASE("client plugin discovery: a user-dir twin of a bundled basename is its own "
          "disabled entry",
          "[client_plugins][discovery]") {
    // Entries are keyed by (file, provenance), not basename alone: a user-dir
    // .so named like a bundled one used to match the bundled entry and
    // inherit its enabled:true -- loading a plugin nobody opted into.
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "bundled/one");
    std::filesystem::create_directories(dir.path() / "user");
    std::ofstream(dir.path() / "bundled/one/twin.so") << "stub";
    std::ofstream(dir.path() / "user/twin.so") << "stub";

    ClientPluginConfig config;
    const auto candidates =
        DiscoverClientPlugins(dir.path() / "bundled", dir.path() / "user", config);
    REQUIRE(candidates.size() == 2);   // two entries, not one shared one
    CHECK(candidates[0].provenance == Provenance::Bundled);
    CHECK(candidates[0].enabled);        // bundled default, its own entry
    CHECK(candidates[1].provenance == Provenance::User);
    CHECK_FALSE(candidates[1].enabled);  // user default -- nothing inherited
    REQUIRE(config.plugins.size() == 2);
    CHECK(config.plugins[0].file == "twin.so");
    CHECK(config.plugins[0].provenance == Provenance::Bundled);
    CHECK(config.plugins[1].file == "twin.so");
    CHECK(config.plugins[1].provenance == Provenance::User);
}

namespace {

// Captures warn-level lines through the DefaultLogger's per-call spdlog
// resolution -- the same path the production warning travels. Restored by the
// RAII guard below so other tests keep the console default.
class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    bool Matches(const std::string& fragment) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& line : lines) {
            if (line.find(fragment) != std::string::npos) return true;
        }
        return false;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        lines.emplace_back(buf.data(), buf.size());
    }
    void flush_() override {}

private:
    std::vector<std::string> lines;
};

struct LogCapture {
    std::shared_ptr<CapturingSink> sink = std::make_shared<CapturingSink>();
    spdlog::sink_ptr previous;

    LogCapture() {
        // spdlog's default logger owns its sinks; grabbing the raw pointer is
        // safe because the old logger object itself is kept alive by the
        // registry until set_default_logger replaces it -- and we restore
        // before this struct dies.
        previous = spdlog::default_logger()->sinks().front();
        auto capture = std::make_shared<spdlog::logger>(
            "capture", std::static_pointer_cast<spdlog::sinks::sink>(sink));
        spdlog::set_default_logger(capture);
    }
    ~LogCapture() {
        auto restore = std::make_shared<spdlog::logger>("minimal", previous);
        spdlog::set_default_logger(restore);
    }

    bool HasWarning(const std::string& fragment) { return sink->Matches(fragment); }
};

}  // namespace

TEST_CASE("client plugin discovery: a stale persisted entry warns nothing once its "
          "file is gone",
          "[client_plugins][discovery][invariant]") {
    // The persisted client-plugins.json is never pruned. The both-provenances
    // collision warning must scan the DISCOVERED files (the server's rule --
    // the client had reintroduced the scan over the persisted list), or a
    // stale user entry for a deleted .so warns forever about a twin that no
    // longer exists.
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "bundled/one");
    std::filesystem::create_directories(dir.path() / "user");
    std::ofstream(dir.path() / "bundled/one/a.so") << "stub";
    // twin.so existed once, was disabled, then deleted: the entry survives.

    ClientPluginConfig config;
    config.plugins.push_back({"a.so", Provenance::Bundled, true});
    config.plugins.push_back({"twin.so", Provenance::User, false});

    bool warned = false;
    {
        LogCapture capture;
        DiscoverClientPlugins(dir.path() / "bundled", dir.path() / "user", config);
        warned = capture.HasWarning("collide");
    }
    CHECK_FALSE(warned);

    // The real case still warns: twin.so present on BOTH sides of the disk.
    std::ofstream(dir.path() / "bundled/one/twin.so") << "stub";
    std::ofstream(dir.path() / "user/twin.so") << "stub";
    bool twin_warned = false;
    {
        LogCapture capture;
        DiscoverClientPlugins(dir.path() / "bundled", dir.path() / "user", config);
        twin_warned = capture.HasWarning("collide");
    }
    CHECK(twin_warned);
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
    const Theme theme = Theme::Parse(doc, ColorDepth::Truecolor);
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
    // The STATUS downgrade of an error result happens server-side (the
    // dispatcher), so the client-side contract is: display survives the trip
    // as set -- UNSPECIFIED is not rewritten here.
    failed.set_display(v1::COMMAND_DISPLAY_STATUS);
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
    REQUIRE(fixtures->records.size() == 1);
    REQUIRE(fixtures->records[0].ok());

    const auto* segment = fixtures->registries.status_segments.Find("fixture_seg");
    REQUIRE(segment != nullptr);

    // Post-load use: the load is long over; the sink must still be alive.
    CHECK(segment->Render("") == "seam-check");
    CHECK(fixtures->attention.TabRaised("seam-session"));
}

#ifndef MINIMAL_LOGO_PLUGIN
#error "MINIMAL_LOGO_PLUGIN must name the built logo plugin"
#endif

// The FIRST bundled client plugin, loaded as a real artifact: proves the
// start-view seam end to end through dlopen, and pins the contract the TUI
// relies on (a start view renders for BOTH the app-level empty state and a
// session, and -- regenerate_per_session being the default -- two sessions do
// not share one banner).
TEST_CASE("the bundled logo plugin loads through the real loader and renders art",
          "[client_plugins][logo]") {
    const auto fixtures = LoadedFixtures::Load({PluginCandidate{
        MINIMAL_LOGO_PLUGIN, Provenance::Bundled, /*enabled=*/true, {}}});
    REQUIRE(fixtures->records.size() == 1);
    const auto& record = fixtures->records.front();
    REQUIRE(record.ok());
    CHECK(record.name == "logo");
    REQUIRE(record.registered.at("start_views").size() == 1);

    const auto* view = fixtures->registries.start_views.Find("logo.banner");
    REQUIRE(view != nullptr);
    const Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);

    const auto app_art = ToText(view->Render("", theme));
    CHECK(!app_art.empty());
    const auto first = ToText(view->Render("session-1", theme));
    const auto second = ToText(view->Render("session-2", theme));
    CHECK(!first.empty());
    CHECK(!second.empty());
    // The per-session seed: each new session keys its own generation. A
    // collision needs identical style AND fill across ~100 cells -- about
    // 2^-29 per draw pair, not a live flake source.
    CHECK(first != second);
}

#ifndef MINIMAL_CONFIG_VIEW_PLUGIN
#error "MINIMAL_CONFIG_VIEW_PLUGIN must name the built config-view plugin"
#endif

// config-view as the real artifact, through the real loader: the "config"
// modal and the /config command both register. Its sources are also compiled
// into the test binary (schema_form_test), which proves the logic but not
// that the bundled module registers anything.
TEST_CASE("the bundled config-view plugin registers its view and /config",
          "[client_plugins][config_view]") {
    const auto fixtures = LoadedFixtures::Load({PluginCandidate{
        MINIMAL_CONFIG_VIEW_PLUGIN, Provenance::Bundled, /*enabled=*/true, {}}});
    REQUIRE(fixtures->records.size() == 1);
    const auto& record = fixtures->records.front();
    INFO(record.error);
    REQUIRE(record.ok());
    CHECK(record.name == "config-view");
    CHECK(fixtures->registries.views.Find("config") != nullptr);
    bool found = false;
    for (const auto& command : fixtures->registries.client_commands.All()) {
        found = found || command->name() == "config";
    }
    CHECK(found);
}

// regenerate_per_session=false must share one banner across every session
// EVEN WHEN the app-level empty state (session id "") is never rendered --
// that used to be the only path that populated the shared cache slot, so a
// caller that jumps straight to a session (e.g. resuming on startup) got a
// fresh banner per session despite the flag.
TEST_CASE("the bundled logo plugin shares one banner when regenerate_per_session is false",
          "[client_plugins][logo]") {
    const auto fixtures = LoadedFixtures::Load({PluginCandidate{
        MINIMAL_LOGO_PLUGIN, Provenance::Bundled, /*enabled=*/true,
        {{"regenerate_per_session", false}}}});
    REQUIRE(fixtures->records.size() == 1);
    REQUIRE(fixtures->records.front().ok());

    const auto* view = fixtures->registries.start_views.Find("logo.banner");
    REQUIRE(view != nullptr);
    const Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);

    const auto first = ToText(view->Render("session-1", theme));
    const auto second = ToText(view->Render("session-2", theme));
    CHECK(!first.empty());
    CHECK(first == second);
}
