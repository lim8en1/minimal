#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "../../plugins/server/tools-bash/bash_state_service.hpp"
#include "../../plugins/server/tools-bash/restart_shell.hpp"
#include "commands_core_fixture.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/service_registry.hpp"

using namespace minimal;
using namespace minimal::testing;
using tools_bash::kBashStateServiceName;

namespace {

// Records what the command asked for, so the test can distinguish "the service
// was called through the registry" from "something reset some state".
class RecordingService : public tools_bash::BashSessionStateService {
public:
    std::vector<std::string> erased;
    int resets = 0;

    void Erase(const std::string& session_id) override { erased.push_back(session_id); }
    void Reset(const std::string&, const std::filesystem::path&) override { ++resets; }
};

}  // namespace

TEST_CASE("Provide then Find round-trips the registered service", "[service_registry]") {
    plugins::ServiceRegistry services;
    RecordingService service;
    services.Provide("a.service", &service);

    REQUIRE(services.Find("a.service") != nullptr);
    CHECK(static_cast<RecordingService*>(services.Find("a.service")) == &service);
}

TEST_CASE("Find before Provide and Find of a name nobody provided are null",
          "[service_registry]") {
    plugins::ServiceRegistry services;
    // Absence is a runtime condition (the provider plugin may be disabled),
    // not an error: null and nothing else, no diagnostic.
    CHECK(services.Find("never.provided") == nullptr);

    RecordingService service;
    services.Provide("a.service", &service);
    CHECK(services.Find("never.provided") == nullptr);  // still, and others unaffected
    CHECK(services.Find("a.service") != nullptr);
}

TEST_CASE("a Provide after Seal is ignored, not registered", "[service_registry]") {
    // After plugin load the map must stay read-only: Find runs unguarded from
    // session workers and the command pool, and a runtime registration would
    // be a data race invisible to collision detection besides. main.cpp seals
    // the registry right after LoadAll; a plugin holding the registration
    // context and calling Provide later costs a warning, never the race.
    plugins::ServiceRegistry services;
    RecordingService service;
    services.Seal();

    services.Provide("late.service", &service);
    CHECK(services.Find("late.service") == nullptr);
    CHECK(services.attributions().empty());
}

TEST_CASE("a null Provide is ignored, not registered", "[service_registry]") {
    // Registering "nothing" would turn Find's null contract ("absent") into a
    // lie; it also must not leave an attribution behind.
    plugins::ServiceRegistry services;
    services.SetAttribution("some-plugin");
    services.Provide("a.service", nullptr);
    services.ClearAttribution();

    CHECK(services.Find("a.service") == nullptr);
    CHECK(services.attributions().empty());
}

TEST_CASE("the attribution window records who provided what", "[service_registry]") {
    plugins::ServiceRegistry services;
    services.SetAttribution("tools-bash");
    RecordingService service;
    services.Provide("a.service", &service);
    services.ClearAttribution();
    services.Provide("unattributed.service", &service);  // outside any window (core itself)

    REQUIRE(services.attributions().size() == 2);
    CHECK(services.attributions()[0].plugin_name == "tools-bash");
    CHECK(services.attributions()[0].service_name == "a.service");
    CHECK(services.attributions()[1].plugin_name.empty());
    CHECK(services.attributions()[1].service_name == "unattributed.service");
}

TEST_CASE("two plugins providing one service name collide, and the resolution names both",
          "[service_registry][collision]") {
    plugins::ServiceRegistry services;
    RecordingService first;
    RecordingService second;
    services.SetAttribution("tools-bash");
    services.Provide("a.service", &first);
    services.SetAttribution("bash_over_ssh");
    services.Provide("a.service", &second);
    services.ClearAttribution();

    const auto resolution = plugins::ResolveRegistrationNames(tools::ToolRegistry{}, {}, nullptr,
                                                              {}, &services);
    REQUIRE(resolution.collisions.size() == 1);
    CHECK(resolution.collisions[0].surface == "a.service");

    // Both claimants survive in the attributions -- the fatal startup diagnostic
    // is built from exactly these pairs, so it can name both plugins.
    REQUIRE(services.attributions().size() == 2);
    CHECK(services.attributions()[0].plugin_name == "tools-bash");
    CHECK(services.attributions()[1].plugin_name == "bash_over_ssh");
}

TEST_CASE("a service name may repeat a tool or command name without colliding",
          "[service_registry][collision]") {
    // Services live in their own namespace: the name is addressed by CODE
    // through Find, never by the model or the user, so there is nothing to be
    // ambiguous about.
    plugins::ServiceRegistry services;
    tools::ToolRegistry tools;
    tools.SetAttribution("tools-bash");
    tools.Register(std::make_unique<FakeTool>("a.service", tools::ToolCategory::Execute));
    tools.ClearAttribution();
    services.SetAttribution("tools-bash");
    RecordingService service;
    services.Provide("a.service", &service);
    services.ClearAttribution();

    const auto resolution =
        plugins::ResolveRegistrationNames(tools, {}, nullptr, {}, &services);
    CHECK(resolution.collisions.empty());
}

TEST_CASE("a service never gains a permission key", "[service_registry]") {
    // Permission keys come from the tool namespace only; a service landing in
    // that map would answer a permission question nobody asked.
    plugins::ServiceRegistry services;
    services.SetAttribution("tools-bash");
    RecordingService service;
    services.Provide("a.service", &service);
    services.ClearAttribution();

    const auto resolution =
        plugins::ResolveRegistrationNames(tools::ToolRegistry{}, {}, nullptr, {}, &services);
    CHECK(resolution.permission_keys.empty());
    // And it is not a dangling-alias false positive either.
    CHECK(resolution.dangling_aliases.empty());
}

TEST_CASE("/restart-shell resets through the service path", "[service_registry][commands]") {
    // The point of track L, asserted rather than asserted about: the command
    // resolves the store through Find -- the same lookup a third-party consumer
    // would use -- not through a plugin-internal static.
    CommandFixture f;  // session_id() is "s1"
    plugins::ServiceRegistry services;
    RecordingService service;
    services.Provide(kBashStateServiceName, &service);
    f.registry.Register(std::make_unique<tools_bash::RestartShellCommand>(&services));

    const auto result = f.Run("restart-shell");

    REQUIRE(result.status == commands::CommandResult::Status::kOk);
    REQUIRE(service.erased.size() == 1);
    CHECK(service.erased.front() == "s1");
    CHECK(service.resets == 0);
}

TEST_CASE("/restart-shell degrades with an error when the service is absent",
          "[service_registry][commands]") {
    // A provider may be disabled or stale (pre-service build): a runtime
    // condition answered with a clear kError, never a throw, never a crash.
    CommandFixture f;
    plugins::ServiceRegistry empty;
    f.registry.Register(std::make_unique<tools_bash::RestartShellCommand>(&empty));

    const auto result = f.Run("restart-shell");
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find(kBashStateServiceName) != std::string::npos);

    // And with NO service seam offered by the host at all (a loader built
    // without the registry): same degrade.
    CommandFixture f2;
    f2.registry.Register(std::make_unique<tools_bash::RestartShellCommand>(nullptr));
    const auto result2 = f2.Run("restart-shell");
    CHECK(result2.status == commands::CommandResult::Status::kError);
}
