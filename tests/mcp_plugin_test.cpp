// The mcp plugin's own pieces, over core headers: config validation, the
// manager limits, the client endpoint, and the channel glue to core.signals.
// (The extern "C" entry points in plugin.cpp stay out, like every bundled
// plugin's.)

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp_channels.hpp"
#include "mcp_plugin_endpoint.hpp"
#include "mcp_settings.hpp"
#include "mcp_validation.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/mcp/channels_config.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/mcp/mcps_config.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;

namespace {

// Records what the glue hands core.signals.
class RecordingSignals final : public agent::SignalService {
public:
    agent::SignalDelivery DeliverInternal(const std::string&, const std::string&,
                                          const std::string&) override {
        return agent::SignalDelivery::kInvalid;
    }
    bool RegisterExternalChannel(const agent::ExternalChannel& channel,
                                 agent::ExternalChannelSource* source,
                                 std::string&) override {
        registered.push_back(channel.name);
        sources.push_back(source);
        return true;
    }
    void SetChannelInstructions(const std::string& channel,
                                const std::string& text) override {
        instructions[channel] = text;
    }
    agent::SignalDelivery DeliverExternal(const std::string& channel, const std::string& signal,
                                          const std::string& content,
                                          const std::map<std::string, std::string>& meta) override {
        delivered.push_back({channel, signal, content, meta});
        return agent::SignalDelivery::kDelivered;
    }

    struct Delivery {
        std::string channel, signal, content;
        std::map<std::string, std::string> meta;
    };
    std::vector<std::string> registered;
    std::vector<agent::ExternalChannelSource*> sources;
    std::map<std::string, std::string> instructions;
    std::vector<Delivery> delivered;
};

bool Mentions(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("ValidateMcpConfig: per transport, disabled skipped, channel conflicts",
          "[mcp_plugin]") {
    const auto servers = config::ParseMcpsConfig(nlohmann::json::array({
        {{"name", "good-stdio"}, {"transport", "stdio"}, {"command", "/bin/echo"}},
        {{"name", "bad-stdio"}, {"transport", "stdio"}, {"command", "definitely-not-a-binary"}},
        {{"name", "good-http"}, {"transport", "http"}, {"url", "https://mcp.example.com/sse"}},
        {{"name", "bad-http"}, {"transport", "http"}, {"url", "ftp://nope"}},
        {{"name", "off"}, {"transport", "stdio"}, {"command", "definitely-not-a-binary"},
         {"enabled", false}},
    }));
    const auto channels = config::ParseChannelsConfig(nlohmann::json{
        {"good-stdio", {{"mode", "extended"}, {"command", "/bin/echo"}}},
        {"ci", {{"mode", "extended"}, {"command", "no-such-channel-binary"}}},
    });

    const auto warnings = mcp::ValidateMcpConfig(servers, channels);
    CHECK(Mentions(warnings, "'bad-stdio'"));
    CHECK(Mentions(warnings, "'bad-http'"));
    CHECK_FALSE(Mentions(warnings, "'good-stdio': command"));
    CHECK_FALSE(Mentions(warnings, "'good-http'"));
    CHECK_FALSE(Mentions(warnings, "'off'"));  // disabled: never spawns
    CHECK(Mentions(warnings, "channel 'ci': command"));
    CHECK(Mentions(warnings, "also an mcps.json server name"));
}

TEST_CASE("ParseMcpLimits: plugin config over legacy tools.mcp over defaults", "[mcp_plugin]") {
    const auto defaults = mcp::ParseMcpLimits(nlohmann::json::object(), nlohmann::json::object());
    CHECK(defaults.connect.count() == 10000);
    CHECK(defaults.max_list_pages == 100);

    const nlohmann::json legacy = {{"mcp", {{"connect_timeout_ms", 1000}, {"max_list_pages", 7}}}};
    const auto from_legacy = mcp::ParseMcpLimits(nlohmann::json::object(), legacy);
    CHECK(from_legacy.connect.count() == 1000);
    CHECK(from_legacy.max_list_pages == 7);

    const auto from_config = mcp::ParseMcpLimits({{"max_list_pages", 3}}, legacy);
    CHECK(from_config.max_list_pages == 3);
    CHECK(from_config.connect.count() == 1000);  // untouched keys still fall back

    CHECK_THROWS(mcp::ParseMcpLimits({{"max_list_pages", 0}}, nlohmann::json::object()));
    CHECK_THROWS(mcp::ParseMcpLimits({{"restart_cooldown_ms", -1}}, nlohmann::json::object()));
    CHECK_THROWS(mcp::ParseMcpLimits({{"connect_timeout_ms", "soon"}}, nlohmann::json::object()));
}

TEST_CASE("mcp.endpoint: list and set_enabled over the manager", "[mcp_plugin]") {
    tools::ToolRegistry registry;
    auto servers = config::ParseMcpsConfig(nlohmann::json::array({
        {{"name", "zeta"}, {"transport", "stdio"}, {"command", "/bin/echo"}, {"enabled", false}},
        {{"name", "alpha"}, {"transport", "http"}, {"url", "https://x.example"}, {"enabled", false}},
    }));
    mcp::McpClientManager manager(std::move(servers), registry, mcp::McpLimits{},
                                  common::DefaultLogger());
    mcp::McpPluginEndpoint endpoint(manager);

    auto list = nlohmann::json::parse(endpoint.Call("list", "{}"));
    REQUIRE(list["servers"].size() == 2);
    CHECK(list["servers"][0]["name"] == "alpha");  // sorted
    CHECK(list["servers"][0]["transport"] == "http");
    CHECK(list["servers"][1]["enabled"] == false);

    auto toggled =
        nlohmann::json::parse(endpoint.Call("set_enabled", R"({"name":"zeta","enabled":true})"));
    CHECK(toggled["ok"] == true);
    CHECK(manager.StatusOf("zeta").enabled);

    auto unknown =
        nlohmann::json::parse(endpoint.Call("set_enabled", R"({"name":"ghost","enabled":true})"));
    CHECK(unknown["ok"] == false);
    CHECK_FALSE(unknown["error"].get<std::string>().empty());

    CHECK_THROWS(endpoint.Call("set_enabled", "{}"));
    CHECK_THROWS(endpoint.Call("frobnicate", "{}"));
    CHECK_FALSE(endpoint.Subscribe("list", "{}", nullptr));
}

TEST_CASE("channel glue: decode, deliver, instructions", "[mcp_plugin][signals]") {
    RecordingSignals signals;
    const auto wiring = mcp::MakeChannelWiring(config::ChannelsConfig{}, &signals);

    wiring.on_notification(
        "ci", nlohmann::json{{"method", "notifications/minimal/signal"},
                             {"params",
                              {{"signal", "build.completed"},
                               {"content", "ok"},
                               {"meta", {{"run", "7"}}}}}}
                  .dump());
    wiring.on_notification("ci", R"({"method":"notifications/tools/list_changed"})");
    wiring.on_notification("ci", "not json");
    REQUIRE(signals.delivered.size() == 1);
    CHECK(signals.delivered[0].channel == "ci");
    CHECK(signals.delivered[0].signal == "build.completed");
    CHECK(signals.delivered[0].meta.at("run") == "7");

    wiring.on_connected("ci", "Build events.");
    CHECK(signals.instructions.at("ci") == "Build events.");

    // No signal service in the host: dropped, not a crash.
    const auto orphan = mcp::MakeChannelWiring(config::ChannelsConfig{}, nullptr);
    orphan.on_notification(
        "ci", R"({"method":"notifications/minimal/signal","params":{"signal":"a","content":"b"}})");
}

TEST_CASE("RegisterChannels: only the channels the manager built, with the manager as source",
          "[mcp_plugin][signals]") {
    tools::ToolRegistry registry;
    auto channels = config::ParseChannelsConfig(nlohmann::json{
        {"ci", {{"mode", "restricted"}, {"signals", {"build.done"}}, {"command", "/bin/echo"}}},
        {"off", {{"mode", "extended"}, {"command", "/bin/echo"}, {"enabled", false}}},
    });
    RecordingSignals signals;
    mcp::McpClientManager manager(config::McpsConfig{}, registry, mcp::McpLimits{},
                                  common::DefaultLogger(),
                                  mcp::MakeChannelWiring(channels, &signals));
    mcp::McpChannelSource source(manager);
    std::vector<std::string> warnings;
    mcp::RegisterChannels(channels, manager, signals, source,
                          [&](const std::string& w) { warnings.push_back(w); });

    CHECK(signals.registered == std::vector<std::string>{"ci"});
    REQUIRE(signals.sources.size() == 1);
    CHECK(signals.sources[0] == &source);
    CHECK(warnings.empty());
}
