#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "minimal/rpc/server_housekeeping.hpp"

using minimal::rpc::ServerHousekeeping;
using namespace std::chrono_literals;

namespace {

// A housekeeping instance whose own thread never ticks during the test, so
// every Tick below is the test's, at the time the test names.
struct Harness {
    std::size_t attached = 0;
    std::chrono::milliseconds client_idle_for = std::chrono::hours(1);
    bool sessions_hold = false;
    int idle_exits = 0;
    int checkpoints = 0;

    ServerHousekeeping::Config Config() {
        ServerHousekeeping::Config config;
        config.idle_shutdown = std::chrono::seconds(1);
        config.attached_clients = [this] { return attached; };
        config.client_idle_for = [this] { return client_idle_for; };
        config.sessions_hold = [this] { return sessions_hold; };
        config.on_idle_expired = [this] { ++idle_exits; };
        config.periodic_checkpoint = [this] { ++checkpoints; };
        config.tick = std::chrono::hours(1);
        return config;
    }
};

const auto kStart = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

}  // namespace

TEST_CASE("the idle exit fires once the quiet state has lasted idle_shutdown",
          "[server_housekeeping]") {
    Harness h;
    ServerHousekeeping housekeeping(h.Config());
    housekeeping.Tick(kStart);
    housekeeping.Tick(kStart + 900ms);
    CHECK(h.idle_exits == 0);
    housekeeping.Tick(kStart + 1100ms);
    CHECK(h.idle_exits == 1);
}

TEST_CASE("the idle exit holds while a session-less client keeps pinging",
          "[server_housekeeping]") {
    // A live client that holds neither a stream nor a session -- the TUI's
    // welcome view -- is only visible through its heartbeat.
    Harness h;
    h.client_idle_for = 0ms;
    ServerHousekeeping housekeeping(h.Config());
    for (int i = 0; i < 40; ++i) housekeeping.Tick(kStart + i * 100ms);
    CHECK(h.idle_exits == 0);
}

TEST_CASE("an attached client or a holding session resets the linger", "[server_housekeeping]") {
    Harness h;
    ServerHousekeeping housekeeping(h.Config());
    housekeeping.Tick(kStart);
    h.sessions_hold = true;
    housekeeping.Tick(kStart + 900ms);
    h.sessions_hold = false;
    housekeeping.Tick(kStart + 1100ms);  // the linger restarted here
    CHECK(h.idle_exits == 0);
    h.attached = 1;
    housekeeping.Tick(kStart + 1500ms);
    h.attached = 0;
    housekeeping.Tick(kStart + 1600ms);
    housekeeping.Tick(kStart + 2500ms);
    CHECK(h.idle_exits == 0);
    housekeeping.Tick(kStart + 2700ms);
    CHECK(h.idle_exits == 1);
}

TEST_CASE("idle_shutdown 0 never exits, and the checkpoint runs every tick",
          "[server_housekeeping]") {
    Harness h;
    auto config = h.Config();
    config.idle_shutdown = 0s;
    ServerHousekeeping housekeeping(std::move(config));
    for (int i = 0; i < 5; ++i) housekeeping.Tick(kStart + i * 10s);
    CHECK(h.idle_exits == 0);
    CHECK(h.checkpoints == 5);
}

TEST_CASE("the housekeeping thread ticks on its own until stopped", "[server_housekeeping]") {
    std::atomic<int> ticks{0};
    ServerHousekeeping::Config config;
    config.periodic_checkpoint = [&ticks] { ticks.fetch_add(1); };
    config.tick = 10ms;
    ServerHousekeeping housekeeping(std::move(config));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (ticks.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    housekeeping.Stop();
    const int after_stop = ticks.load();
    CHECK(after_stop >= 3);
    std::this_thread::sleep_for(50ms);
    CHECK(ticks.load() == after_stop);
}
