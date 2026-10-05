// The /-command system: registry, dispatcher, and the bundled commands.
//
// Driven against a fake SessionControl, which is the reason CommandContext takes
// an interface rather than agent::Session -- a command needs a preset, a loop and
// an executor, not a worker thread, a gRPC stream and a SQLite transcript.

#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "commands_core_fixture.hpp"
#include "temp_dir.hpp"
#include "minimal/plugins/name_resolution.hpp"

using namespace minimal;
using namespace minimal::testing;
using Status = minimal::commands::CommandResult::Status;

namespace {
// RAII env override, matching config_paths_test.cpp's convention.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) { had_old_ = true; old_ = old; }
        ::setenv(name, value, 1);
    }
    ~ScopedEnv() {
        if (had_old_) ::setenv(name_.c_str(), old_.c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
private:
    std::string name_;
    bool had_old_ = false;
    std::string old_;
};
}  // namespace

TEST_CASE("an unknown command is answered, not dropped", "[commands]") {
    // A client waiting on a CommandResult hangs otherwise -- the same reasoning
    // that made TurnCompletedEvent unconditional.
    CommandFixture fixture;
    const auto result = fixture.Run("nope");
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("/nope") != std::string::npos);
    CHECK(result.message.find("/help") != std::string::npos);
}

TEST_CASE("help lists every registered command and explains one", "[commands]") {
    CommandFixture fixture;

    const auto listing = fixture.Run("help");
    REQUIRE(listing.status == Status::kOk);
    for (const char* name : {"/help", "/compact", "/clear", "/agent", "/model",
                             "/subagent-model"}) {
        INFO("expected " << name << " in the listing");
        CHECK(listing.message.find(name) != std::string::npos);
    }

    const auto one = fixture.Run("help", {"model"});
    CHECK(one.message.find("[provider/model]") != std::string::npos);
    const auto subagent_one = fixture.Run("help", {"subagent-model"});
    CHECK(subagent_one.message.find("inherit") != std::string::npos);
}

TEST_CASE("a user-chosen model survives an agent switch", "[commands][invariant]") {
    // THE PRECEDENCE RULE, and the bug it exists to prevent: /agent re-resolves
    // the preset from disk, so without re-applying the override it reverts to
    // whatever that preset names. This shipped broken and a manual run caught it
    // -- the unit tests were green, and the success message printed a bare model
    // id, identical for both references, which hid the revert.
    CommandFixture fixture;

    REQUIRE(fixture.Run("model", {"main/slow"}).status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/slow");

    const auto switched = fixture.Run("agent", {"other"});  // `other` names main/fast
    REQUIRE(switched.status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/slow");
    CHECK(switched.message.find("main/slow") != std::string::npos);
}

TEST_CASE("agent without an override adopts the preset's model", "[commands]") {
    // The other half of the rule: a preset IS where the default comes from, so
    // with nothing chosen the switch must actually take effect.
    CommandFixture fixture;
    REQUIRE(fixture.Run("agent", {"other"}).status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/fast");
    CHECK(fixture.preset().name == "other");
}

TEST_CASE("bare /agent opens the picker view rather than switching or erroring",
          "[commands]") {
    // Mirrors /sessions' view_hint mapping: the command carries no data, the
    // client fetches the preset list itself over ListAgentPresets.
    CommandFixture fixture;
    const auto result = fixture.Run("agent");
    CHECK(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kView);
    CHECK(result.view_hint == "agents");
    CHECK(fixture.preset().name == "build");  // unchanged
}

TEST_CASE("bare /model opens the model picker rather than erroring", "[commands]") {
    // The replacement for /providers: interactive selection lives in the
    // client's searchable view, fetched by the client itself over ListModels.
    CommandFixture fixture;
    const auto result = fixture.Run("model");
    CHECK(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kView);
    CHECK(result.view_hint == "models");
    CHECK(fixture.preset().name == "build");          // unchanged
    CHECK_FALSE(fixture.model_override().has_value());  // nothing switched
}

TEST_CASE("model switch by reference reports the status line, not the picker view",
          "[commands][regression]") {
    // ModelCommand's static Display()/ViewHint() (kView/"models") describe
    // the command for the catalog; DisplayFor/ViewHintFor make the
    // picker-vs-status choice per-invocation, exactly like AgentCommand's --
    // without which a successful "/model <ref>" switch ALSO reported kView
    // and the client reopened the picker it was just answered from.
    CommandFixture fixture;
    const auto result = fixture.Run("model", {"main/slow"});
    REQUIRE(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kStatus);
    CHECK(result.view_hint.empty());
    CHECK(fixture.preset().model_ref.str() == "main/slow");
}

TEST_CASE("agent switch by name reports the status line, not the picker view",
          "[commands][regression]") {
    // AgentCommand's static Display()/ViewHint() (kView/"agents") describe
    // the command for the catalog, where /agent AS A WHOLE is view-capable
    // -- the dispatcher used to apply that unconditionally to every
    // invocation, so a successful "/agent <name>" switch ALSO reported
    // kView, and the client reopened the picker it was just answered from
    // right after the switch took effect. DisplayFor/ViewHintFor make the
    // picker-vs-status choice per-invocation, keyed on whether a name was
    // given.
    CommandFixture fixture;
    const auto result = fixture.Run("agent", {"other"});
    REQUIRE(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kStatus);
    CHECK(result.view_hint.empty());
}

TEST_CASE("bare /subagent-model opens its picker rather than erroring", "[commands]") {
    // The /model shape: the command carries no data, the client fetches the
    // list itself over ListModels and prepends the "same as parent" row.
    CommandFixture fixture;
    const auto result = fixture.Run("subagent-model");
    CHECK(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kView);
    CHECK(result.view_hint == "subagent-models");
    CHECK_FALSE(fixture.subagent_model_override().has_value());  // nothing set
}

TEST_CASE("/subagent-model <ref> sets the choice; inherit clears it", "[commands]") {
    CommandFixture fixture;

    const auto set = fixture.Run("subagent-model", {"main/slow"});
    REQUIRE(set.status == Status::kOk);
    // A set is a status-line answer, never the picker view -- the same
    // DisplayFor rule /model's regression test pins.
    CHECK(set.display == commands::CommandDisplay::kStatus);
    CHECK(set.view_hint.empty());
    REQUIRE(fixture.subagent_model_override().has_value());
    CHECK(fixture.subagent_model_override()->str() == "main/slow");
    // The session's own model is untouched: this choice governs spawned
    // children only.
    CHECK(fixture.preset().model_ref.str() == "main/fast");

    const auto cleared = fixture.Run("subagent-model", {"inherit"});
    REQUIRE(cleared.status == Status::kOk);
    CHECK(cleared.display == commands::CommandDisplay::kStatus);
    CHECK_FALSE(fixture.subagent_model_override().has_value());
}

TEST_CASE("/subagent-model refuses malformed and unknown refs", "[commands]") {
    CommandFixture fixture;

    const auto malformed = fixture.Run("subagent-model", {"noslash"});
    CHECK(malformed.status == Status::kError);
    CHECK(malformed.message.find("inherit") != std::string::npos);
    CHECK_FALSE(fixture.subagent_model_override().has_value());

    const auto unknown = fixture.Run("subagent-model", {"main/ghost"});
    CHECK(unknown.status == Status::kError);
    CHECK(unknown.message.find("unknown model") != std::string::npos);
    CHECK_FALSE(fixture.subagent_model_override().has_value());
}

TEST_CASE("bare /mcp opens the mcp picker rather than erroring", "[commands]") {
    // Same shape as bare /agent //model: no data carried, the client's
    // mcp-view fetches the server list itself over the mcp plugin's endpoint.
    CommandFixture fixture;
    fixture.SetupMcp(nlohmann::json::array());
    const auto result = fixture.Run("mcp");
    CHECK(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kView);
    CHECK(result.view_hint == "mcp");
}

TEST_CASE("without the mcp plugin there is no /mcp", "[commands]") {
    // /mcp belongs to the mcp plugin: core's own catalog does not carry it.
    CommandFixture fixture;
    CHECK(fixture.registry.Find("mcp") == nullptr);
    CHECK(fixture.Run("mcp", {"anything", "enable"}).status == Status::kError);
}

TEST_CASE("/mcp <name> enable toggles a disabled server live", "[commands]") {
    CommandFixture fixture;
    nlohmann::json disabled = {{"name", "fake"},
                               {"transport", "stdio"},
                               {"command", MINIMAL_MCP_FAKE_SERVER},
                               {"args", {"normal"}},
                               {"enabled", false}};
    fixture.SetupMcp(nlohmann::json::array({disabled}));
    CHECK_FALSE(fixture.mcp_manager()->StatusOf("fake").enabled);

    const auto result = fixture.Run("mcp", {"fake", "enable"});
    REQUIRE(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kStatus);
    CHECK(fixture.mcp_manager()->StatusOf("fake").enabled);
}

TEST_CASE("/mcp <name> enable|disable rejects a malformed action", "[commands]") {
    CommandFixture fixture;
    fixture.SetupMcp(nlohmann::json::array());
    const auto result = fixture.Run("mcp", {"fake", "sideways"});
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("usage") != std::string::npos);
}

TEST_CASE("/mcp <name> enable on an unknown server reports the error", "[commands]") {
    CommandFixture fixture;
    fixture.SetupMcp(nlohmann::json::array());
    const auto result = fixture.Run("mcp", {"ghost", "enable"});
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("not configured") != std::string::npos);
}

TEST_CASE("agent reports an unknown preset rather than throwing", "[commands]") {
    CommandFixture fixture;
    const auto result = fixture.Run("agent", {"nonexistent"});
    CHECK(result.status == Status::kError);
    CHECK(fixture.preset().name == "build");  // unchanged
}

TEST_CASE("switching to a discovered model resolves it lazily, window and all",
          "[commands][discover]") {
    // Track N through the surviving surface: with /providers gone, a
    // discover-entry's models are reached by /model directly. Resolve is
    // what triggers the lazy discovery fetch (a listing never does), and
    // the discovered context window rides ApplyModel into the preset --
    // which is what the compaction trigger reads.
    CommandFixture fixture;
    fixture.WriteProviders(R"({"providers":{"main":{"backend":"fake","url":"http://x/v1",
                              "models":{"fast":{"context_window_tokens":1000}}},
                             "disc":{"backend":"fake","url":"http://disc/v1",
                                     "discover":true}}})");
    fixture.ReloadPresets();

    auto hook_models = std::vector<llm::DiscoveredModel>{
        {"newmodel", 12345, nlohmann::json::object(), std::nullopt}};
    int calls = 0;
    fixture.SetDiscoveryHook(
        [&calls, &hook_models](const std::string&, const llm::ProviderConfig&) {
            ++calls;
            return config::DiscoveryOutcome{/*catalog_present=*/true, /*ok=*/true, {},
                                            hook_models};
        });

    // The switch resolves disc/newmodel, which IS the lazy discovery fetch.
    const auto switched = fixture.Run("model", {"disc/newmodel"});
    REQUIRE(switched.status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "disc/newmodel");
    REQUIRE(fixture.preset().context_window_tokens.has_value());
    CHECK(*fixture.preset().context_window_tokens == 12345);
    CHECK(calls == 1);  // cached; a second switch of the same ref refetches nothing

    // An unknown ref under a warm discovery failure is an honest error, and
    // neither touched the session beyond the first switch.
    const auto unknown = fixture.Run("model", {"disc/nope"});
    CHECK(unknown.status == Status::kError);
    CHECK(calls == 1);
    CHECK(fixture.preset().model_ref.str() == "disc/newmodel");
}

TEST_CASE("model refuses what it cannot resolve", "[commands]") {    CommandFixture fixture;

    const auto malformed = fixture.Run("model", {"slow"});
    CHECK(malformed.status == Status::kError);
    CHECK(malformed.message.find("<provider>/<model>") != std::string::npos);

    const auto unknown = fixture.Run("model", {"main/nope"});
    CHECK(unknown.status == Status::kError);

    // Neither touched the session.
    CHECK(fixture.preset().model_ref.str() == "main/fast");
    CHECK_FALSE(fixture.model_override().has_value());
}

TEST_CASE("a confirming command runs only on yes", "[commands][invariant]") {
    CommandFixture fixture;
    fixture.SeedHistory();
    REQUIRE(fixture.loop().history().size() == 2);

    SECTION("declined leaves the session alone") {
        fixture.confirmations.answer = "No";
        const auto result = fixture.Run("clear");
        CHECK(result.status == Status::kOk);
        CHECK(result.message.find("cancelled") != std::string::npos);
        CHECK(fixture.loop().history().size() == 2);
    }

    SECTION("no answer at all is reported, not treated as a decline") {
        // Cancelled, or the session is going away. Not a choice the user made,
        // so it is surfaced rather than silently doing nothing.
        fixture.confirmations.answer = "";
        const auto result = fixture.Run("clear");
        CHECK(result.status == Status::kError);
        CHECK(fixture.loop().history().size() == 2);
    }

    SECTION("confirmed runs it") {
        fixture.confirmations.answer = "Yes";
        CHECK(fixture.Run("clear").status == Status::kOk);
        CHECK(fixture.loop().history().empty());
    }
}

TEST_CASE("clear drops context but never the transcript", "[commands][invariant]") {
    // The rule compaction established: what the model sees and what scrollback
    // shows are different things. Clearing the first must not touch the second.
    CommandFixture fixture;
    fixture.SeedHistory();
    const auto stored_before = fixture.stored_messages();
    REQUIRE(stored_before == 2);

    fixture.confirmations.answer = "Yes";
    REQUIRE(fixture.Run("clear").status == Status::kOk);

    CHECK(fixture.loop().history().empty());
    CHECK(fixture.stored_messages() == stored_before);
}

TEST_CASE("an LLM command is refused while a turn is in flight", "[commands]") {
    // Queueing it behind the turn would make /compact run later against a
    // context that had moved on, which is worse than saying no.
    CommandFixture fixture;
    const auto result = fixture.Run("compact", {}, /*busy=*/true);
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("in flight") != std::string::npos);
}

TEST_CASE("a read-only command still answers while a turn is in flight",
          "[commands][invariant]") {
    // The whole reason ReadOnly() is on the interface: these two must be
    // answerable beside a live turn, and the dispatcher is what allows it.
    CommandFixture fixture;
    CHECK(fixture.Run("help", {}, /*busy=*/true).status == Status::kOk);
    CHECK(fixture.Run("sessions", {}, /*busy=*/true).status == Status::kOk);
}

TEST_CASE("a throwing command does not take the worker down", "[commands]") {
    // The session survives a failed turn; it survives a failed command for the
    // same reason.
    CommandFixture fixture;
    fixture.registry.Register(std::make_unique<ThrowingCommand>());
    const auto result = fixture.Run("boom");
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("deliberate") != std::string::npos);
}

// --- Registry: aliasing and collision reporting ------------------------------

TEST_CASE("an alias renames a command's surface, keeping its identity",
          "[commands][invariant]") {
    // plugins.json's aliases were parsed and re-serialized for the whole
    // life of the plugin system without ever being APPLIED to anything. Keys are
    // "plugin.original_name", so they are unambiguous even before collisions are
    // resolved -- which is what lets aliasing resolve one.
    commands::CommandRegistry registry;
    registry.SetAliases({{"commands-core.compact", "c"}});
    registry.SetAttribution("commands-core");
    for (auto& command : commands_core::MakeCoreCommands()) {
        registry.Register(std::move(command));
    }
    registry.ClearAttribution();

    CHECK(registry.Find("c") != nullptr);        // the surface name
    CHECK(registry.Find("compact") == nullptr);  // no longer typed as this

    bool found = false;
    for (const auto& attribution : registry.attributions()) {
        if (attribution.original_name != "compact") continue;
        found = true;
        CHECK(attribution.plugin_name == "commands-core");
        CHECK(attribution.surface_name == "c");  // identity kept, surface moved
    }
    CHECK(found);
}

TEST_CASE("a duplicate surface name is recorded on both sides", "[commands][invariant]") {
    // Recorded rather than resolved: the collision check needs BOTH culprits to
    // name them, and a registry that dropped the loser would leave it with one.
    // Silent shadowing makes behaviour depend on plugin load order, which is
    // exactly what the fatal rule exists to prevent.
    commands::CommandRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(std::make_unique<ThrowingCommand>());
    registry.ClearAttribution();
    registry.SetAttribution("plugin-b");
    registry.Register(std::make_unique<ThrowingCommand>());
    registry.ClearAttribution();

    REQUIRE(registry.attributions().size() == 2);
    CHECK(registry.attributions()[0].plugin_name == "plugin-a");
    CHECK(registry.attributions()[1].plugin_name == "plugin-b");
    CHECK(registry.attributions()[0].surface_name == registry.attributions()[1].surface_name);
}

TEST_CASE(
    "RemoveByAttribution erases the winner's slot even when a loser's attribution survives",
    "[commands][invariant][regression]") {
    // The bug: the "claimed" check considered ANY surviving attribution for
    // the surface name, including a losing collision attempt -- so a losing
    // plugin's mere registration attempt blocked erasure of the WINNING
    // plugin's command during that winning plugin's own rollback. Here
    // plugin-a wins "boom"; plugin-b's later attempt loses but is still
    // recorded. Rolling back plugin-a (the actual owner of the commands_
    // slot) must erase "boom", regardless of plugin-b's leftover record.
    commands::CommandRegistry registry;
    registry.SetAttribution("plugin-a");
    registry.Register(std::make_unique<ThrowingCommand>());
    registry.ClearAttribution();
    registry.SetAttribution("plugin-b");
    registry.Register(std::make_unique<ThrowingCommand>());
    registry.ClearAttribution();
    REQUIRE(registry.Find("boom") != nullptr);

    registry.RemoveByAttribution("plugin-a");

    CHECK(registry.Find("boom") == nullptr);
    // plugin-b's losing attribution is still recorded for diagnostics.
    REQUIRE(registry.attributions().size() == 1);
    CHECK(registry.attributions()[0].plugin_name == "plugin-b");

    // The inverse must also hold: removing a LOSER's own attribution must
    // never erase a slot it never owned.
    commands::CommandRegistry registry2;
    registry2.SetAttribution("plugin-a");
    registry2.Register(std::make_unique<ThrowingCommand>());
    registry2.ClearAttribution();
    registry2.SetAttribution("plugin-b");
    registry2.Register(std::make_unique<ThrowingCommand>());
    registry2.ClearAttribution();

    registry2.RemoveByAttribution("plugin-b");
    CHECK(registry2.Find("boom") != nullptr);
}

// --- The collision check, which had never run ---------------------------------

TEST_CASE("duplicate surface names are fatal, naming both plugins",
          "[commands][plugins][invariant]") {
    // Silent shadowing would make which plugin wins depend on directory
    // iteration order.
    tools::ToolRegistry tools;
    commands::CommandRegistry commands;

    commands.SetAttribution("plugin-a");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();
    commands.SetAttribution("plugin-b");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();

    const auto names = plugins::ResolveRegistrationNames(tools, {}, &commands);
    REQUIRE(names.collisions.size() == 1);
    CHECK(names.collisions[0].surface == "boom");

    // The collision names both claimants -- the surface name alone does not
    // say which plugin to disable.
    CHECK(names.collisions[0].space == plugins::NameSpace::kCommand);
    CHECK(names.collisions[0].claimants == std::vector<std::string>{"plugin-a", "plugin-b"});
}

TEST_CASE("an alias resolves a collision rather than merely avoiding one",
          "[commands][plugins][invariant]") {
    // Aliases are applied BEFORE the check, which is what makes aliasing a valid
    // remedy. Applied after, the collision would already be fatal and the alias
    // would be useless for the one job it was documented to do.
    tools::ToolRegistry tools;
    commands::CommandRegistry commands;
    commands.SetAliases({{"plugin-b.boom", "boom2"}});

    commands.SetAttribution("plugin-a");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();
    commands.SetAttribution("plugin-b");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();

    const auto names =
        plugins::ResolveRegistrationNames(tools, {{"plugin-b.boom", "boom2"}}, &commands);
    CHECK(names.collisions.empty());
    CHECK(names.dangling_aliases.empty());
    // Both are reachable, under different names.
    CHECK(commands.Find("boom") != nullptr);
    CHECK(commands.Find("boom2") != nullptr);
}

TEST_CASE("an alias naming nothing registered is reported", "[commands][plugins]") {
    // Detectable at load, which is the argument for aliases overriding metadata
    // of code-registered entries rather than declaring commands as data.
    tools::ToolRegistry tools;
    commands::CommandRegistry commands;
    commands.SetAttribution("plugin-a");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();

    const auto names =
        plugins::ResolveRegistrationNames(tools, {{"plugin-a.nonexistent", "x"}}, &commands);
    REQUIRE(names.dangling_aliases.size() == 1);
    CHECK(names.dangling_aliases[0] == "plugin-a.nonexistent");
}

TEST_CASE("/compact works when the automatic trigger is Never", "[commands][invariant]") {
    // CROSS-FEATURE, and the seam the two branches could not have tested
    // together: the trigger became a strategy whose default is Never, and
    // /compact deliberately bypasses it -- the user asked, so occupancy and the
    // configured strategy are beside the point. Routing /compact through
    // MaybeCompact instead of Compact would make it silently do nothing on the
    // default configuration, which is the exact shape of the bug H shipped.
    CommandFixture fixture;
    fixture.SeedHistory();
    fixture.SeedHistory();
    REQUIRE(fixture.loop().history().size() == 4);

    // LoopConfig's default trigger is Never, so nothing automatic can fire.
    const auto result = fixture.Run("compact");
    REQUIRE(result.status == Status::kOk);
    CHECK(result.message.find("compacted") != std::string::npos);
    CHECK(fixture.loop().history().size() < 4);
}

TEST_CASE("/compact with a named prompt folds history under THAT prompt",
          "[commands][compact]") {
    CommandFixture fixture;
    fixture.SeedHistory();
    fixture.SeedHistory();
    REQUIRE(fixture.loop().history().size() == 4);

    const auto result = fixture.Run("compact", {"terse"});
    REQUIRE(result.status == Status::kOk);
    CHECK(result.message.find("compacted") != std::string::npos);
    CHECK(fixture.loop().history().size() < 4);
    // The named file's body reached the compaction call, not the default.
    REQUIRE(fixture.last_compaction_prompt().has_value());
    CHECK(*fixture.last_compaction_prompt() == "TERSE.\n");
}

TEST_CASE("bare /compact still uses the preset's compression prompt", "[commands][compact]") {
    CommandFixture fixture;
    fixture.SeedHistory();
    fixture.SeedHistory();

    const auto result = fixture.Run("compact");
    REQUIRE(result.status == Status::kOk);
    REQUIRE(fixture.last_compaction_prompt().has_value());
    CHECK(*fixture.last_compaction_prompt() == "SUMMARIZE.\n");
}

TEST_CASE("/compact rejects an unknown or invalid prompt name without compacting",
          "[commands][compact]") {
    CommandFixture fixture;
    fixture.SeedHistory();
    fixture.SeedHistory();

    // A name with no prompts/<name>.md behind it: the registry's throw becomes
    // a clean command error, and the history is untouched.
    const auto missing = fixture.Run("compact", {"nonexistent"});
    CHECK(missing.status == Status::kError);
    CHECK(missing.message.find("nonexistent") != std::string::npos);
    CHECK(fixture.loop().history().size() == 4);
    CHECK(!fixture.last_compaction_prompt().has_value());

    // The same bare-filename rule the frontmatter field enforces: a traversal
    // attempt is refused, never resolved relative to somewhere else.
    const auto traversal = fixture.Run("compact", {"../prompts/terse"});
    CHECK(traversal.status == Status::kError);
    CHECK(traversal.message.find("must be a file name") != std::string::npos);
    CHECK(fixture.loop().history().size() == 4);
}

TEST_CASE("/compact refuses more than one prompt argument", "[commands][compact]") {
    CommandFixture fixture;
    fixture.SeedHistory();
    fixture.SeedHistory();

    const auto result = fixture.Run("compact", {"terse", "other"});
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("usage") != std::string::npos);
    CHECK(fixture.loop().history().size() == 4);
}

// ---------------------------------------------------------------------------
// The sessionless dispatch path (RunSessionlessCommand RPC): a NULL session
// ref in the CommandContext, refused for everything but a Sessionless<>
// command. This is the guard the sessionless-tab TUI flow leans on.
// ---------------------------------------------------------------------------

TEST_CASE("sessionless dispatch answers help and the view-only commands",
          "[commands][sessionless]") {
    CommandFixture fixture;
    minimal::commands::CommandArgs args;
    minimal::commands::CommandContext context(nullptr, fixture.registry_presets_for_test(),
                                              fixture.registry_confirmations_for_test(),
                                              common::DefaultLogger());

    const auto help = commands::DispatchSessionless(fixture.registry, "help", args, context);
    REQUIRE(help.status == Status::kOk);
    CHECK(help.message.find("/compact") != std::string::npos);

    const auto sessions = commands::DispatchSessionless(fixture.registry, "sessions", args, context);
    REQUIRE(sessions.status == Status::kOk);
    // The view shaping is the point: the client opens its picker from THIS.
    CHECK(sessions.display == minimal::commands::CommandDisplay::kView);
    CHECK(sessions.view_hint == "sessions");

    const auto unknown = commands::DispatchSessionless(fixture.registry, "nope", args, context);
    CHECK(unknown.status == Status::kError);
    CHECK(unknown.message.find("unknown command") != std::string::npos);
}

TEST_CASE("sessionless dispatch refuses session-bound commands with the shared wording",
          "[commands][sessionless]") {
    CommandFixture fixture;
    minimal::commands::CommandArgs args;
    minimal::commands::CommandContext context(nullptr, fixture.registry_presets_for_test(),
                                              fixture.registry_confirmations_for_test(),
                                              common::DefaultLogger());

    // /clear is a sessioned HarnessCommand: refused WITHOUT executing (the
    // fake session must stay untouched, not just answer an error).
    const auto clear = commands::DispatchSessionless(fixture.registry, "clear", args, context);
    REQUIRE(clear.status == Status::kError);
    CHECK(clear.message == commands::SessionRequiredMessage("clear"));
    CHECK(fixture.loop().history().empty());

    const auto agent = commands::DispatchSessionless(fixture.registry, "agent", args, context);
    CHECK(agent.status == Status::kError);

    // RequiresSession defaults to true, so a plugin command that never heard
    // of the mixin is session-bound by construction -- and a sessioned
    // dispatch still runs it exactly as before.
    CHECK(commands::SessionRequiredMessage("x") == "/x needs a session; send a message to start one first");
}

TEST_CASE("sessionless dispatch executes /secret -- a Sessionless command must be "
          "ReadOnly too, or the advertised command cannot run anywhere",
          "[commands][sessionless][secret]") {
    // THE /connect REGRESSION, kept for its successor: a command that opted
    // into Sessionless<> (the client catalog then offers it on sessionless
    // tabs) but kept the default ReadOnly()=false was refused by
    // DispatchSessionless with the session-required wording -- advertised
    // pre-session, runnable only on a sessioned tab, backwards for a command
    // whose purpose is storing a key BEFORE any session exists.
    CommandFixture fixture;
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    const auto secrets_path = data.path() / "minimal" / "secrets.json";

    minimal::commands::CommandArgs args;
    args.positional = {"set", "local", "sk-test-123"};
    minimal::commands::CommandContext context(nullptr, fixture.registry_presets_for_test(),
                                              fixture.registry_confirmations_for_test(),
                                              common::DefaultLogger());

    const auto result = commands::DispatchSessionless(fixture.registry, "secret", args, context);
    REQUIRE(result.status == Status::kOk);
    REQUIRE(std::filesystem::exists(secrets_path));
    const nlohmann::json stored = nlohmann::json::parse(std::ifstream(secrets_path));
    CHECK(stored.at("local").at("value") == "sk-test-123");
}

// --- /secret --------------------------------------------------------------------

TEST_CASE("/secret set, list and delete manage secrets.json and refuse junk",
          "[commands][secret]") {
    CommandFixture fixture;
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    const auto secrets_path = data.path() / "minimal" / "secrets.json";

    CHECK(fixture.Run("secret", {"list"}).message.find("no secrets stored") != std::string::npos);

    REQUIRE(fixture.Run("secret", {"set", "local", "sk-test-123"}).status == Status::kOk);
    // Merge, not replace: a second secret keeps the first; --host binds.
    REQUIRE(fixture.Run("secret", {"set", "remote", "key-2", "--host", "api.example.com",
                                   "--host", "127.0.0.1:8080"})
                .status == Status::kOk);
    nlohmann::json stored = nlohmann::json::parse(std::ifstream(secrets_path));
    CHECK(stored.at("local").at("value") == "sk-test-123");
    CHECK_FALSE(stored.at("local").contains("hosts"));
    CHECK(stored.at("remote").at("value") == "key-2");
    CHECK(stored.at("remote").at("hosts") ==
          nlohmann::json::array({"api.example.com", "127.0.0.1:8080"}));

    // list shows names and bindings, never a value.
    const auto listed = fixture.Run("secret", {"list"});
    CHECK(listed.status == Status::kOk);
    CHECK(listed.message.find("local") != std::string::npos);
    CHECK(listed.message.find("api.example.com") != std::string::npos);
    CHECK(listed.message.find("sk-test-123") == std::string::npos);
    CHECK(listed.message.find("key-2") == std::string::npos);

    // Set again replaces the entry whole: the binding goes with it.
    REQUIRE(fixture.Run("secret", {"set", "remote", "key-3"}).status == Status::kOk);
    stored = nlohmann::json::parse(std::ifstream(secrets_path));
    CHECK(stored.at("remote").at("value") == "key-3");
    CHECK_FALSE(stored.at("remote").contains("hosts"));

    REQUIRE(fixture.Run("secret", {"delete", "remote"}).status == Status::kOk);
    stored = nlohmann::json::parse(std::ifstream(secrets_path));
    CHECK_FALSE(stored.contains("remote"));
    CHECK(stored.contains("local"));
    CHECK(fixture.Run("secret", {"delete", "remote"}).status == Status::kError);

    // Usage and validation: no silent junk in the secrets file. ':' would
    // read as a plugin namespace ("secret:<plugin>:NAME"), so it is refused.
    CHECK(fixture.Run("secret", {}).status == Status::kError);
    CHECK(fixture.Run("secret", {"set", "local"}).status == Status::kError);
    CHECK(fixture.Run("secret", {"set", "a:b", "key"}).status == Status::kError);
    CHECK(fixture.Run("secret", {"set", "x", "key", "--host"}).status == Status::kError);
    CHECK(fixture.Run("secret", {"set", "x", "key", "--hots", "h"}).status == Status::kError);
    // A URL as --host could never match a destination.
    CHECK(fixture.Run("secret", {"set", "x", "key", "--host", "https://api.example.com"})
              .status == Status::kError);
    CHECK(fixture.Run("secret", {"set", "x", "key", "--host", "[::1]:8080"}).status ==
          Status::kOk);

    // A store that parses but is not an object is refused, not replaced.
    std::ofstream(secrets_path) << "[]";
    CHECK(fixture.Run("secret", {"set", "y", "key"}).status == Status::kError);
    CHECK(nlohmann::json::parse(std::ifstream(secrets_path)) == nlohmann::json::array());
    // An empty one reads as no secrets.
    std::ofstream(secrets_path, std::ios::trunc).flush();
    CHECK(fixture.Run("secret", {"set", "y", "key"}).status == Status::kOk);
}

TEST_CASE("/secret writes secrets.json 0600, never group/other-readable",
          "[commands][secret]") {
    CommandFixture fixture;
    testing::TempDir data;
    // A root no other test case can have already written into: the assertion
    // is specifically about a FRESH file's mode.
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());
    const auto secrets_path = data.path() / "minimal" / "secrets.json";

    REQUIRE(fixture.Run("secret", {"set", "local", "sk-test-123"}).status == Status::kOk);
    REQUIRE(std::filesystem::exists(secrets_path));

    struct stat st {};
    REQUIRE(::stat(secrets_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0600);
}

TEST_CASE("/secret reports a failed save as a save failure, not a lock failure",
          "[commands][secret]") {
    // Regression: one catch around lock AND save labelled every exception
    // "could not lock ... for update". A value that is not valid UTF-8 cannot
    // be serialized as JSON -- that is a save failure.
    CommandFixture fixture;
    testing::TempDir data;
    ScopedEnv data_env("XDG_DATA_HOME", data.path().string().c_str());

    const auto result = fixture.Run("secret", {"set", "local", "sk-\xff"});
    CAPTURE(result.message);
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("could not update") != std::string::npos);
    CHECK(result.message.find("could not lock") == std::string::npos);
}

TEST_CASE("/agent, /model, /subagent-model and /think refuse extra arguments",
          "[commands]") {
    CommandFixture fixture;
    CHECK(fixture.Run("agent", {"a", "b"}).status == Status::kError);
    CHECK(fixture.Run("model", {"local/m", "extra"}).status == Status::kError);
    CHECK(fixture.Run("subagent-model", {"inherit", "extra"}).status == Status::kError);
    CHECK(fixture.Run("think", {"high", "extra"}).status == Status::kError);
}

// /rename takes the name from the RAW tail (a name may contain spaces), so
// these tests bypass the fixture's positional-only Run() helper and build the
// args the wire actually carries.
namespace {
commands::CommandResult RunRename(CommandFixture& fixture, const std::string& raw) {
    commands::CommandArgs args;
    args.raw = raw;
    commands::CommandContext context(&fixture, fixture.registry_presets_for_test(),
                                     fixture.confirmations, common::DefaultLogger());
    return commands::Dispatch(fixture.registry, "rename", args, context, /*busy=*/false);
}
}  // namespace

TEST_CASE("/rename trims the raw tail and hands the whole name to the session", "[commands]") {
    CommandFixture fixture;
    const auto result = RunRename(fixture, "  fix the login bug  ");
    REQUIRE(result.status == Status::kOk);
    REQUIRE(fixture.renamed_to().has_value());
    CHECK(*fixture.renamed_to() == "fix the login bug");
}

TEST_CASE("/rename with a blank name refuses with the usage, touching nothing", "[commands]") {
    // The raw tail is ALL whitespace -- no name to speak of. The session must
    // never see an empty name (SessionControl::RenameSession refuses those
    // anyway; the usage line is the better answer).
    CommandFixture fixture;
    const auto result = RunRename(fixture, "   \t ");
    CHECK(result.status == Status::kError);
    CHECK(result.message.find("/rename") != std::string::npos);
    CHECK_FALSE(fixture.renamed_to().has_value());
}

TEST_CASE("/rename surfaces a refused rename as a command error", "[commands]") {
    // A failed persistence (unknown session, database error) must not read as
    // success: the client's notice line is the only acknowledgment there is.
    CommandFixture fixture;
    fixture.rename_ok = false;
    const auto result = RunRename(fixture, "new name");
    CHECK(result.status == Status::kError);
    CHECK(fixture.renamed_to().has_value());   // the session was asked
    CHECK(*fixture.renamed_to() == "new name");
}

TEST_CASE("bare /subagents opens the subagent picker rather than erroring", "[commands]") {
    // The /sessions shape: no data carried, the client fetches the parent's
    // children itself over ListSessions(include_subagents).
    CommandFixture fixture;
    const auto result = fixture.Run("subagents");
    CHECK(result.status == Status::kOk);
    CHECK(result.display == commands::CommandDisplay::kView);
    CHECK(result.view_hint == "subagents");
}

// --- /think: the effort-variant switcher -------------------------------------

TEST_CASE("/think lists the active model's declared variants and marks the current one",
          "[commands][think]") {
    CommandFixture fixture;
    // Rebuild the config dir's providers.json with a variant-bearing model and
    // re-resolve, exactly like the fixture's refresh tests do.
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":["low","high"]}}}}})");
    fixture.ReloadPresets();

    const auto listing = fixture.Run("think");
    REQUIRE(listing.status == Status::kOk);
    CHECK(listing.message.find("main/fast") != std::string::npos);
    CHECK(listing.message.find("low") != std::string::npos);
    CHECK(listing.message.find("high") != std::string::npos);
    // The session is on the BASE model here (the fixture default), so nothing
    // carries the current-variant marker yet.
    CHECK(listing.message.find("(current)") == std::string::npos);
}

TEST_CASE("/think switches to a declared variant; with no /model pin it records nothing",
          "[commands][think][invariant]") {
    // Pin only what was pinned: without a /model choice the variant changes
    // the running preset's model and nothing more, so /agent afterwards runs
    // the new preset's own model instead of this session's old one.
    CommandFixture fixture;
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":["high"]},
                       "slow":{"context_window_tokens":2000}}}}})");
    fixture.ReloadPresets();

    const auto switched = fixture.Run("think", {"high"});
    REQUIRE(switched.status == Status::kOk);
    CHECK(switched.message.find("high") != std::string::npos);
    CHECK(fixture.preset().model_ref.str() == "main/fast@high");
    // The wire id is the BASE's: the suffix never reaches the provider.
    CHECK(fixture.preset().provider.model_name == "fast");
    // The derived parameter rides.
    CHECK(fixture.preset().parameters.at("reasoning_effort") == "high");
    CHECK_FALSE(fixture.model_override().has_value());

    REQUIRE(fixture.Run("agent", {"other"}).status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() != "main/fast@high");
}

TEST_CASE("/think after /model replaces the pinned choice, which survives /agent",
          "[commands][think][invariant]") {
    CommandFixture fixture;
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":["high"]},
                       "slow":{"context_window_tokens":2000}}}}})");
    fixture.ReloadPresets();
    REQUIRE(fixture.Run("model", {"main/fast"}).status == Status::kOk);

    REQUIRE(fixture.Run("think", {"high"}).status == Status::kOk);
    REQUIRE(fixture.model_override().has_value());
    CHECK(fixture.model_override()->str() == "main/fast@high");

    REQUIRE(fixture.Run("agent", {"other"}).status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/fast@high");
}

TEST_CASE("/think off returns to the base model", "[commands][think]") {
    CommandFixture fixture;
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":["high"]}}}}})");
    fixture.ReloadPresets();
    REQUIRE(fixture.Run("think", {"high"}).status == Status::kOk);

    const auto off = fixture.Run("think", {"off"});
    REQUIRE(off.status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/fast");
    CHECK(fixture.preset().parameters.find("reasoning_effort") ==
          fixture.preset().parameters.end());
}

TEST_CASE("/think refuses an undeclared variant by name, listing what exists",
          "[commands][think]") {
    CommandFixture fixture;
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":["low"]}}}}})");
    fixture.ReloadPresets();

    const auto unknown = fixture.Run("think", {"max"});
    CHECK(unknown.status == Status::kError);
    CHECK(unknown.message.find("max") != std::string::npos);
    CHECK(unknown.message.find("low") != std::string::npos);

    // A model with no variants at all says so instead of pretending.
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000}}}}})");
    fixture.ReloadPresets();
    const auto none = fixture.Run("think");
    REQUIRE(none.status == Status::kOk);
    CHECK(none.message.find("declares no reasoning variants") != std::string::npos);
}

TEST_CASE("/think off switches to a declared variant literally named \"off\" instead of "
         "resetting to the base model",
         "[commands][think]") {
    // The object form of "variants" accepts any non-empty, "@"-free name
    // with an explicit parameter body (unlike the array form, which is
    // restricted to the known effort ladder) -- "off" is unusual but valid.
    // The hardcoded "off means reset" shortcut must not shadow a declared
    // variant actually named that.
    CommandFixture fixture;
    fixture.WriteProviders(
        R"({"providers":{"main":{"backend":"openai","url":"http://x/v1",
             "models":{"fast":{"context_window_tokens":1000,
                               "variants":{"off":{"parameters":{"x":1}}}}}}}})");
    fixture.ReloadPresets();

    const auto switched = fixture.Run("think", {"off"});
    REQUIRE(switched.status == Status::kOk);
    CHECK(fixture.preset().model_ref.str() == "main/fast@off");
}

TEST_CASE("/permissions shows the policy, dry-runs a call and revokes grants",
          "[commands][permissions]") {
    CommandFixture fixture;
    fixture.tools_for_test().Register(
        std::make_unique<testing::FakeTool>("write", tools::ToolCategory::FileWrite, "ok"));
    tools::PermissionPolicy plan;
    plan.file_write = tools::PermissionMode::AlwaysDeny;
    fixture.SetPoliciesForTest(plan);

    const auto shown = fixture.Run("permissions");
    REQUIRE(shown.status == Status::kOk);
    CHECK(shown.message.find("file_write=deny") != std::string::npos);
    CHECK(shown.message.find("session grants (0)") != std::string::npos);

    // The dry run goes through the real gate and names the deciding rule.
    const auto checked = fixture.Run("permissions", {"check", "write", "{}"});
    REQUIRE(checked.status == Status::kOk);
    CHECK(checked.message == "write: deny -- category: file_write=deny");
    CHECK(fixture.Run("permissions", {"check", "nope"}).status == Status::kError);
    CHECK(fixture.Run("permissions", {"check", "write", "{not json"}).status == Status::kError);

    CHECK(fixture.Run("permissions", {"revoke", "bash:/usr/bin/git"}).status == Status::kError);
    const auto all = fixture.Run("permissions", {"revoke", "all"});
    REQUIRE(all.status == Status::kOk);
    CHECK(all.message == "revoked 0 grant(s)");
}

TEST_CASE("picker commands answer their bare form beside a live turn; the argument form "
          "keeps the command's own ReadOnly",
          "[commands]") {
    CommandFixture fixture;
    const commands::CommandArgs bare;
    commands::CommandArgs with_arg;
    with_arg.positional = {"x"};
    for (const char* name : {"agent", "model", "subagent-model"}) {
        CAPTURE(name);
        const auto* command = fixture.registry.Find(name);
        REQUIRE(command != nullptr);
        CHECK_FALSE(command->ReadOnly());          // catalog + subagent guard: unchanged
        CHECK(command->ReadOnlyFor(bare));         // opens a picker: touches nothing
        CHECK_FALSE(command->ReadOnlyFor(with_arg));  // mutates: turn boundary
    }
    fixture.SetupMcp(nlohmann::json::array());
    const auto* mcp = fixture.registry.Find("mcp");
    REQUIRE(mcp != nullptr);
    CHECK(mcp->ReadOnlyFor(bare));
    CHECK(mcp->ReadOnlyFor(with_arg));
    // Non-picker commands: the static answer, whatever the args.
    const auto* clear = fixture.registry.Find("clear");
    REQUIRE(clear != nullptr);
    CHECK_FALSE(clear->ReadOnlyFor(bare));
}

// --- /trust-directory ----------------------------------------------------------

TEST_CASE("/trust-directory asks, then trusts the project and /untrust-directory revokes it",
          "[commands][trust]") {
    CommandFixture fixture;
    auto& trust = fixture.UseProjectTrust(config::ProjectLocalMode::kTrusted);
    const auto root = fixture.project_root();
    REQUIRE_FALSE(trust.IsTrusted(root));

    fixture.confirmations.answer = "Cancel";
    CHECK(fixture.Run("trust-directory").status == Status::kOk);
    CHECK_FALSE(trust.IsTrusted(root));  // declined: nothing recorded

    fixture.confirmations.answer = "";   // no answer is not consent
    CHECK(fixture.Run("trust-directory").status == Status::kError);
    CHECK_FALSE(trust.IsTrusted(root));

    fixture.confirmations.answer = "Trust";
    const auto granted = fixture.Run("trust-directory");
    CHECK(granted.status == Status::kOk);
    CHECK(trust.IsTrusted(root));
    // The prompt named the directory and what would be enabled.
    CHECK(fixture.confirmations.last.title.find(root.string()) != std::string::npos);
    CHECK(fixture.confirmations.last.detail.find("found now") != std::string::npos);

    const int asked = fixture.confirmations.calls;
    CHECK(fixture.Run("trust-directory").status == Status::kOk);  // already trusted
    CHECK(fixture.confirmations.calls == asked);                   // and not asked again

    CHECK(fixture.Run("untrust-directory").status == Status::kOk);
    CHECK_FALSE(trust.IsTrusted(root));
}

TEST_CASE("/trust-directory explains modes where there is nothing to grant", "[commands][trust]") {
    SECTION("all") {
        CommandFixture fixture;
        fixture.UseProjectTrust(config::ProjectLocalMode::kAll);
        CHECK(fixture.Run("trust-directory").status == Status::kOk);
        CHECK(fixture.confirmations.calls == 0);
    }
    SECTION("none") {
        CommandFixture fixture;
        auto& trust = fixture.UseProjectTrust(config::ProjectLocalMode::kNone);
        CHECK(fixture.Run("trust-directory").status == Status::kError);
        CHECK_FALSE(trust.IsTrusted(fixture.project_root()));
    }
    SECTION("no gate configured") {
        CommandFixture fixture;
        CHECK(fixture.Run("trust-directory").status == Status::kError);
    }
}
