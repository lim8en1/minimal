#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "minimal/config/channels_config.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/mcp/mcp_connection.hpp"
#include "minimal/mcp/signal_protocol.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "support/deadline.hpp"

using namespace std::chrono_literals;

namespace {

constexpr const char* kFakeServer = MINIMAL_MCP_FAKE_SERVER;
constexpr const char* kFakeChannel = MINIMAL_SIGNAL_CHANNEL_SERVER;

using minimal::mcp::McpConnection;
using minimal::mcp::McpLimits;

McpLimits TestLimits() { return McpLimits{std::chrono::seconds(10), std::chrono::seconds(10)}; }

minimal::mcp::NotificationSink Collect(std::vector<std::string>& sink,
                                        std::mutex& mu,
                                        std::condition_variable& cv) {
    return [&sink, &mu, &cv](const std::string& raw) {
        std::lock_guard<std::mutex> lock(mu);
        sink.push_back(raw);
        cv.notify_all();
    };
}

// Poll until the predicate holds, bounded -- a hang is a failure, not a
// wait. Local because deadline.hpp's BackgroundResult solves a different
// shape (blocking CALLS under a deadline, not predicate polling).
template <typename Predicate>
bool WaitUntil(Predicate ready, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

}  // namespace

// The CONNECTION-level notification path, exercised against the scripted
// channel server: capability declared, instructions captured, exactly the
// signal frame delivered to the sink -- raw, as the read thread carried it.
TEST_CASE("McpConnection: a channel server's notifications reach the sink", "[mcp][signals]") {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> received;

    McpConnection::Failure error;
    minimal::mcp::McpEndpoint endpoint;
    endpoint.transport = minimal::mcp::McpTransport::Stdio;
    endpoint.command_line = kFakeChannel;
    auto connection = McpConnection::Open(
        endpoint, TestLimits(), "test", "0.0.1", error, Collect(received, mu, cv));
    REQUIRE(connection);
    if (!connection) return;

    // The handshake result: the capability the manager greps for, and the
    // instructions a session's context carries.
    const auto capabilities =
        nlohmann::json::parse(connection->ServerCapabilitiesJson(), nullptr, false);
    REQUIRE(capabilities.is_object());
    REQUIRE(capabilities.contains("experimental"));
    CHECK(capabilities.at("experimental").contains("minimal/signal"));
    CHECK(connection->Instructions().find("Build events") != std::string::npos);

    // Discovery, exactly as the manager's StartHandle runs it: the scripted
    // server emits its one signal right after answering tools/list -- the
    // deterministic emission point (an idle client, past initialize).
    McpConnection::Failure tools_error;
    const auto tools = connection->ListTools(nullptr, tools_error);
    REQUIRE(tools.has_value());
    CHECK(tools->empty());  // a channel offers no tools

    // Exactly the scripted frame arrives, raw.
    REQUIRE(WaitUntil([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !received.empty();
    }));
    std::string frame;
    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(received.size() == 1);
        frame = received.front();
    }
    const auto parsed = nlohmann::json::parse(frame, nullptr, false);
    REQUIRE(parsed.is_object());
    CHECK(parsed.at("method").get<std::string>() ==
          std::string(minimal::mcp::kSignalNotificationMethod));
    CHECK(parsed.at("params").at("signal").get<std::string>() == "build.completed");
    CHECK(parsed.at("params").at("content").get<std::string>() == "build 123 passed");
}

// A connection opened WITHOUT a sink behaves exactly as before: notifications
// are dropped (logged) and nothing else changes -- the no-channels default.
TEST_CASE("McpConnection: no sink is the pre-channels behavior", "[mcp][signals]") {
    McpConnection::Failure error;
    minimal::mcp::McpEndpoint endpoint;
    endpoint.transport = minimal::mcp::McpTransport::Stdio;
    endpoint.command_line = kFakeChannel;
    auto connection = McpConnection::Open(endpoint, TestLimits(), "test", "0.0.1", error);
    REQUIRE(connection);
    if (!connection) return;
    // Capability still visible (detection is independent of the sink)...
    CHECK(connection->ServerCapabilitiesJson().find("minimal/signal") != std::string::npos);
    // ...and the connection stays alive past the notification the server emits.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(connection->Ping(nullptr, error));
}

// The MANAGER-level channel path: channels.json wiring joins the same
// lifecycle, capability is enforced at connect, and notifications route
// through the wiring with their channel name attached.
TEST_CASE("McpClientManager: channel entries connect, verify, and route", "[mcp][signals]") {
    minimal::tools::ToolRegistry registry;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> routed;  // (channel, raw)

    minimal::mcp::ChannelWiring wiring;
    wiring.on_notification = [&](const std::string& channel, const std::string& raw) {
        std::lock_guard<std::mutex> lock(mu);
        routed.emplace_back(channel, raw);
        cv.notify_all();
    };
    std::atomic<int> connects{0};
    std::string connected_instructions;
    wiring.on_connected = [&](const std::string&, const std::string& instructions) {
        connected_instructions = instructions;
        ++connects;
    };

    // channels.json: one real channel entry.
    const minimal::config::ChannelsConfig channels =
        minimal::config::ParseChannelsConfig(nlohmann::json{
            {"ci", {{"mode", "restricted"},
                    {"signals", {"build.completed", "build.failed"}},
                    {"command", kFakeChannel}}}});

    // mcps.json: a NON-channel stdio server, to prove a channels.json entry
    // whose server never declares the capability fails loudly.
    const std::string mcps_path = "/tmp/minimal_mcp_channel_test_" +
                                  std::to_string(getpid()) + "_mcps.json";
    {
        auto file = std::ofstream(mcps_path);
        file << nlohmann::json::array({nlohmann::json{
            {"name", "plain"}, {"transport", "stdio"}, {"command", kFakeServer},
            {"args", {"normal"}}, {"enabled", true}}})
                .dump();
    }

    minimal::mcp::McpLimits limits = TestLimits();
    minimal::mcp::McpClientManager manager(minimal::config::LoadMcpsConfig(mcps_path), registry,
                                           limits, minimal::common::DefaultLogger(),
                                           minimal::mcp::ChannelWiring{channels, wiring.on_notification,
                                                                       wiring.on_connected});
    std::remove(mcps_path.c_str());

    // First EnsureAlive from a subscribing session lazily starts the channel.
    std::string alive_error;
    REQUIRE(manager.EnsureAlive("ci", alive_error));
    REQUIRE(WaitUntil([&] { return connects.load() >= 1; }));
    CHECK(connected_instructions.find("Build events") != std::string::npos);

    // The notification routes with the channel name attached.
    REQUIRE(WaitUntil([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !routed.empty();
    }));
    CHECK(routed.front().first == "ci");
    CHECK(routed.front().second.find("build.completed") != std::string::npos);

    // A second EnsureAlive reuses the live connection (no restart storm), and
    // the registry got no tools from a tool-less channel.
    REQUIRE(manager.EnsureAlive("ci", alive_error));
    CHECK(connects.load() == 1);
}

// A channel name colliding with an ENABLED mcps.json server: the server wins
// the name, the channel is skipped -- EnabledChannelNames() must agree with
// the manager's own handles_, since that is what it is built from.
TEST_CASE("McpClientManager: EnabledChannelNames omits a channel colliding with an enabled server",
          "[mcp][signals]") {
    minimal::tools::ToolRegistry registry;
    minimal::mcp::ChannelWiring wiring;
    wiring.config = minimal::config::ParseChannelsConfig(
        nlohmann::json{{"dup", {{"mode", "extended"}, {"command", kFakeChannel}}}});

    const std::string mcps_path =
        "/tmp/minimal_mcp_channel_test_" + std::to_string(getpid()) + "_collide.json";
    {
        auto file = std::ofstream(mcps_path);
        file << nlohmann::json::array({nlohmann::json{{"name", "dup"},
                                                       {"transport", "stdio"},
                                                       {"command", kFakeServer},
                                                       {"enabled", true}}})
                    .dump();
    }
    minimal::mcp::McpLimits limits = TestLimits();
    minimal::mcp::McpClientManager manager(minimal::config::LoadMcpsConfig(mcps_path), registry,
                                           limits, minimal::common::DefaultLogger(),
                                           std::move(wiring));
    std::remove(mcps_path.c_str());

    const auto names = manager.EnabledChannelNames();
    CHECK(std::find(names.begin(), names.end(), "dup") == names.end());
}

// A channel name matching a DISABLED mcps.json entry IS still a real
// collision -- a disabled entry gets a handle too (SetEnabled can flip it
// live, without a restart), so the name stays reserved for it exactly like an
// enabled entry's would. The server wins the name; the channel is skipped.
TEST_CASE("McpClientManager: EnabledChannelNames omits a channel colliding with a disabled server",
          "[mcp][signals]") {
    minimal::tools::ToolRegistry registry;
    minimal::mcp::ChannelWiring wiring;
    wiring.config = minimal::config::ParseChannelsConfig(
        nlohmann::json{{"dup", {{"mode", "extended"}, {"command", kFakeChannel}}}});

    const std::string mcps_path =
        "/tmp/minimal_mcp_channel_test_" + std::to_string(getpid()) + "_nocollide.json";
    {
        auto file = std::ofstream(mcps_path);
        file << nlohmann::json::array({nlohmann::json{{"name", "dup"},
                                                       {"transport", "stdio"},
                                                       {"command", kFakeServer},
                                                       {"enabled", false}}})
                    .dump();
    }
    minimal::mcp::McpLimits limits = TestLimits();
    minimal::mcp::McpClientManager manager(minimal::config::LoadMcpsConfig(mcps_path), registry,
                                           limits, minimal::common::DefaultLogger(),
                                           std::move(wiring));
    std::remove(mcps_path.c_str());

    const auto names = manager.EnabledChannelNames();
    CHECK(std::find(names.begin(), names.end(), "dup") == names.end());
}

TEST_CASE("McpClientManager: a channel entry whose server lacks the capability fails loudly",
          "[mcp][signals]") {
    minimal::tools::ToolRegistry registry;
    minimal::mcp::ChannelWiring wiring;
    wiring.config = minimal::config::ParseChannelsConfig(nlohmann::json{
        {"fake", {{"mode", "extended"}, {"command", kFakeServer}, {"args", {"plain"}}}}});

    const std::string mcps_path = "/tmp/minimal_mcp_channel_test_" +
                                  std::to_string(getpid()) + "_nocap.json";
    {
        auto file = std::ofstream(mcps_path);
        file << nlohmann::json::array().dump();
    }
    minimal::mcp::McpLimits limits = TestLimits();
    minimal::mcp::McpClientManager manager(minimal::config::LoadMcpsConfig(mcps_path), registry,
                                           limits, minimal::common::DefaultLogger(),
                                           std::move(wiring));
    std::remove(mcps_path.c_str());

    std::string error;
    CHECK_FALSE(manager.EnsureAlive("fake", error));
    CHECK(error.find("minimal/signal") != std::string::npos);
    // And it stays refused: the second call lands in restart cooldown, naming
    // that instead of re-attempting a connect that will fail the same way.
    CHECK_FALSE(manager.EnsureAlive("fake", error));
    CHECK(error.find("cooldown") != std::string::npos);
}
