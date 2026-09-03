// The /-command system: registry, dispatcher, and the bundled commands.
//
// Driven against a fake SessionControl, which is the reason CommandContext takes
// an interface rather than agent::Session -- a command needs a preset, a loop and
// an executor, not a worker thread, a gRPC stream and a SQLite transcript.

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "commands_core_fixture.hpp"
#include "minimal/plugins/name_resolution.hpp"

using namespace minimal;
using namespace minimal::testing;
using Status = minimal::commands::CommandResult::Status;

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
                             "/permission-mode"}) {
        INFO("expected " << name << " in the listing");
        CHECK(listing.message.find(name) != std::string::npos);
    }

    const auto one = fixture.Run("help", {"model"});
    CHECK(one.message.find("[provider/model]") != std::string::npos);
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
        {"newmodel", 12345, nlohmann::json::object()}};
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

TEST_CASE("permission-mode validates its argument", "[commands]") {
    CommandFixture fixture;
    fixture.confirmations.answer = "Yes";

    CHECK(fixture.Run("permission-mode", {"bypass"}).status == Status::kOk);
    CHECK(fixture.executor().run_mode() == tools::PermissionRunMode::Bypass);

    CHECK(fixture.Run("permission-mode", {"sideways"}).status == Status::kError);
    CHECK(fixture.executor().run_mode() == tools::PermissionRunMode::Bypass);  // unchanged
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

// --- The collision check, which had never run ---------------------------------

TEST_CASE("duplicate surface names are fatal, naming both plugins",
          "[commands][plugins][invariant]") {
    // NameRegistry has existed and been unit-tested since the plugin work
    // landed, but nothing outside its own tests ever CONSTRUCTED one -- so the
    // rule ARCHITECTURE.md calls fatal was never enforced, and both registries
    // shadowed duplicates silently via insert_or_assign. Which plugin won
    // depended on directory iteration order.
    tools::ToolRegistry tools;
    commands::CommandRegistry commands;

    commands.SetAttribution("plugin-a");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();
    commands.SetAttribution("plugin-b");
    commands.Register(std::make_unique<ThrowingCommand>());
    commands.ClearAttribution();

    const auto names = plugins::ResolveRegistrationNames(tools, {}, &commands);
    REQUIRE(names.result.fatal_collisions.size() == 1);
    CHECK(names.result.fatal_collisions[0] == "boom");

    // Both claimants are in the entries, which is what lets the sanity check
    // name them -- the surface name alone does not say which plugin to disable.
    int claimants = 0;
    for (const auto& entry : names.result.entries) {
        if (entry.surface_name == "boom") ++claimants;
    }
    CHECK(claimants == 2);
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
    CHECK(names.result.fatal_collisions.empty());
    CHECK(names.result.dangling_aliases.empty());
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
    REQUIRE(names.result.dangling_aliases.size() == 1);
    CHECK(names.result.dangling_aliases[0] == "plugin-a.nonexistent");
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
