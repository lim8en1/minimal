// The sidecar core: the generic process launcher (pipe plans, passthrough
// fds), the drain-and-reap runner (sinks, timeout, cancellation), the
// background-task registry (accounting, injection, containment), and the
// lifetime watcher. The process tests fork real children, exactly like the
// tools-bash tests.

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "minimal/sidecar/capped_buffer.hpp"
#include "minimal/sidecar/drain.hpp"
#include "minimal/sidecar/lifetime_watch.hpp"
#include "minimal/sidecar/process.hpp"
#include "minimal/sidecar/task_registry.hpp"

using namespace std::chrono_literals;
using namespace minimal;
using namespace minimal::sidecar;
using tools::BackgroundTaskEvent;

namespace {

// Polls a predicate with a deadline -- the standard shape for waiting on a
// detached watcher's observable effects without sleeping a fixed time.
template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

LaunchSpec SimpleSpec(const std::string& script) {
    LaunchSpec spec;
    spec.argv = {"/bin/sh", "-c", script};
    spec.cwd = std::filesystem::temp_directory_path();
    spec.pipes = {PipePlan{PipePlan::ChildEnd::kWrite,
                           {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}}};
    return spec;
}

DrainOutcome RunToCompletion(const std::string& script, std::int64_t timeout_ms = 10000,
                             const StopCheck& stop = {}) {
    auto launched = Launch(SimpleSpec(script));
    REQUIRE(launched.ok);
    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = 64 * 1024;
    DrainOptions options;
    options.timeout_ms = timeout_ms;
    return DrainAndReap(std::move(launched), std::move(sinks), options, stop);
}

}  // namespace

TEST_CASE("Launch runs a child and the drain captures merged output", "[sidecar][process]") {
    const auto outcome = RunToCompletion("echo hello; echo oops >&2; exit 0");
    CHECK_FALSE(outcome.timed_out);
    CHECK_FALSE(outcome.cancelled);
    CHECK_FALSE(outcome.reap_error);
    CHECK(WIFEXITED(outcome.wait_status));
    CHECK(WEXITSTATUS(outcome.wait_status) == 0);
    CHECK(outcome.sinks[0].text == "hello\noops\n");
}

TEST_CASE("A passthrough child fd (CLOEXEC cleared) survives exec", "[sidecar][process]") {
    // fd 3 is the pattern the python interpreter rides on: a pipe end that
    // must survive exec, kept distinct from the real stdout/stderr. Without
    // the explicit clear, exec closes it and the write fails silently.
    LaunchSpec spec;
    spec.argv = {"/bin/sh", "-c", "echo proto >&3; echo diag"};
    spec.cwd = std::filesystem::temp_directory_path();
    spec.pipes = {
        PipePlan{PipePlan::ChildEnd::kWrite, {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
        PipePlan{PipePlan::ChildEnd::kWrite, {{3, true}}},
    };

    auto launched = Launch(spec);
    REQUIRE(launched.ok);
    REQUIRE(launched.parent_fds.size() == 2);

    std::vector<PipeSink> sinks(2);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = 4096;
    sinks[1].mode = PipeSink::Mode::kCapped;
    sinks[1].cap = 4096;
    DrainOptions options;
    options.timeout_ms = 10000;
    const auto outcome =
        DrainAndReap(std::move(launched), std::move(sinks), options, {});
    REQUIRE_FALSE(outcome.reap_error);
    CHECK(WIFEXITED(outcome.wait_status));
    CHECK(WEXITSTATUS(outcome.wait_status) == 0);
    CHECK(outcome.sinks[0].text == "diag\n");
    CHECK(outcome.sinks[1].text == "proto\n");
}

TEST_CASE("A child whose argv[0] needs PATH resolution is resolved in the parent",
          "[sidecar][process]") {
    LaunchSpec spec;
    spec.argv = {"sh", "-c", "echo via-path"};
    spec.cwd = std::filesystem::temp_directory_path();
    spec.pipes = {PipePlan{PipePlan::ChildEnd::kWrite,
                           {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}}};
    auto launched = Launch(spec);
    REQUIRE(launched.ok);

    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = 4096;
    DrainOptions options;
    options.timeout_ms = 10000;
    const auto outcome = DrainAndReap(std::move(launched), std::move(sinks), options, {});
    CHECK(outcome.sinks[0].text == "via-path\n");
}

TEST_CASE("Launch refuses a program that is not on PATH, before forking",
          "[sidecar][process]") {
    LaunchSpec spec;
    spec.argv = {"definitely-not-a-program-xyz", "-c", "true"};
    spec.cwd = std::filesystem::temp_directory_path();
    auto launched = Launch(spec);
    CHECK_FALSE(launched.ok);
    CHECK(launched.spawn_error.find("definitely-not-a-program-xyz") != std::string::npos);
}

TEST_CASE("DrainAndReap kills the group at the deadline and reports the timeout",
          "[sidecar][drain]") {
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = RunToCompletion("sleep 60", 200);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK(outcome.timed_out);
    // Bounded: the kill + bounded reap must not wait out the child's sleep.
    CHECK(elapsed < 5000);
    CHECK(WIFSIGNALED(outcome.wait_status));
    CHECK(WTERMSIG(outcome.wait_status) == SIGKILL);
}

TEST_CASE("DrainAndReap honors the stop check (cancellation)", "[sidecar][drain]") {
    std::atomic<bool> stop_now{false};
    // Flip the stop flag shortly after the child starts.
    std::thread flipper([&] {
        std::this_thread::sleep_for(100ms);
        stop_now.store(true);
    });
    const auto outcome = RunToCompletion("sleep 60", 60000, [&] { return stop_now.load(); });
    flipper.join();
    CHECK(outcome.cancelled);
    CHECK(WIFSIGNALED(outcome.wait_status));
}

TEST_CASE("The bounded sink latches overflow and discards wholesale",
          "[sidecar][drain]") {
    LaunchSpec spec;
    spec.argv = {"/bin/sh", "-c", "yes overflow-marker | head -c 300000"};
    spec.cwd = std::filesystem::temp_directory_path();
    spec.pipes = {PipePlan{PipePlan::ChildEnd::kWrite,
                           {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}}};
    auto launched = Launch(spec);
    REQUIRE(launched.ok);

    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kBounded;
    sinks[0].cap = 1000;
    DrainOptions options;
    options.timeout_ms = 10000;
    const auto outcome = DrainAndReap(std::move(launched), std::move(sinks), options, {});
    CHECK_FALSE(outcome.timed_out);
    CHECK(outcome.sinks[0].overflow);
    // All-or-nothing: no truncated prefix is ever adopted.
    CHECK(outcome.sinks[0].text.empty());
    CHECK(outcome.sinks[0].dropped > 0);
}

TEST_CASE("A discarded sink still drains (the child must not block on a full pipe)",
          "[sidecar][drain]") {
    auto launched = Launch(SimpleSpec("yes discarded | head -c 2000000"));
    REQUIRE(launched.ok);
    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kDiscard;
    DrainOptions options;
    options.timeout_ms = 10000;
    const auto outcome = DrainAndReap(std::move(launched), std::move(sinks), options, {});
    CHECK_FALSE(outcome.timed_out);
    CHECK_FALSE(outcome.reap_error);
    CHECK(WIFEXITED(outcome.wait_status));
    CHECK(outcome.sinks[0].text.empty());
}

TEST_CASE("The capped emit tap sees live chunks", "[sidecar][drain]") {
    auto launched = Launch(SimpleSpec("echo one; echo two"));
    REQUIRE(launched.ok);
    std::mutex mu;
    std::string live;
    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = 4096;
    sinks[0].emit = [&](std::string_view chunk) {
        std::lock_guard<std::mutex> lock(mu);
        live.append(chunk);
    };
    DrainOptions options;
    options.timeout_ms = 10000;
    const auto outcome = DrainAndReap(std::move(launched), std::move(sinks), options, {});
    std::lock_guard<std::mutex> lock(mu);
    CHECK(live.find("one") != std::string::npos);
    CHECK(live.find("two") != std::string::npos);
    CHECK(outcome.sinks[0].text == "one\ntwo\n");
}

TEST_CASE("CappedBuffer keeps the head and the tail with an omission marker",
          "[sidecar][capped_buffer]") {
    CappedBuffer buffer(100);
    for (int i = 0; i < 100; ++i) buffer.Append("0123456789");  // 1000 bytes total
    const auto rendered = buffer.Render();
    // Head is 1/4, tail 3/4 of the cap.
    CHECK(rendered.starts_with("0123456789"));
    CHECK(rendered.ends_with("0123456789"));
    CHECK(rendered.find("bytes omitted") != std::string::npos);
}

namespace {

// A WatchHandles bundle whose sinks capture events and injections -- the
// lifetime-safe seams the registry's watchers write through.
struct CapturingHandles {
    TaskRegistry::WatchHandles handles;
    mutable std::mutex mu;
    mutable std::vector<std::pair<std::string, BackgroundTaskEvent::State>> events;
    mutable std::vector<std::pair<std::string, bool>> injected;  // (text, is_error)

    CapturingHandles() {
        handles.logger = nullptr;
        handles.schedule_injection = [this](llm::Message message) {
            std::lock_guard<std::mutex> lock(mu);
            // The result rides the ToolResultBlock; dig out its text.
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                    std::string text;
                    for (const auto& part : result->content) {
                        text += std::get<llm::TextBlock>(part).text;
                    }
                    injected.emplace_back(std::move(text), result->is_error);
                }
            }
            return true;
        };
        handles.emit_task_event = [this](const BackgroundTaskEvent& event) {
            std::lock_guard<std::mutex> lock(mu);
            events.emplace_back(event.task_id, event.state);
        };
    }

    std::vector<std::pair<std::string, BackgroundTaskEvent::State>> Events() const {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }
};

}  // namespace

TEST_CASE("TaskRegistry runs a body, reports STARTED then the final event, and injects",
          "[sidecar][task_registry]") {
    TaskRegistry registry;
    CapturingHandles captor;

    TaskRegistry::StartArgs args;
    args.session_id = "s1";
    args.label = "echo demo";
    args.id_prefix = "bg";
    args.body = [&](TaskRegistry::TaskState& task, const TaskRegistry::WatchHandles& h) {
        TaskRegistry::Finish(task, h, BackgroundTaskEvent::State::kCompleted,
                             "[background task " + task.id + " completed]", false);
    };
    args.handles = captor.handles;

    const auto id = registry.Start(std::move(args), nullptr);
    REQUIRE(id);

    CHECK(WaitFor([&] {
        for (const auto& info : registry.List("s1")) {
            if (info.id == *id) {
                return info.state == BackgroundTaskEvent::State::kCompleted;
            }
        }
        return false;
    }));

    const auto events = captor.Events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].second == BackgroundTaskEvent::State::kStarted);
    CHECK(events[1].second == BackgroundTaskEvent::State::kCompleted);
    CHECK(registry.CountRunning("s1") == 0);
    REQUIRE(captor.injected.size() == 1);
    CHECK(captor.injected[0].first.find("completed") != std::string::npos);
}

TEST_CASE("TaskRegistry enforces the per-session running limit", "[sidecar][task_registry]") {
    TaskRegistry registry;
    CapturingHandles captor;
    std::atomic<bool> release{false};

    auto start_blocking = [&](std::string* error) {
        TaskRegistry::StartArgs args;
        args.session_id = "s1";
        args.label = "sleep";
        args.id_prefix = "bg";
        args.max_running = 1;
        args.body = [&](TaskRegistry::TaskState& task, const TaskRegistry::WatchHandles& h) {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(5ms);
            }
            // A body that returns without Finish leaves its record kStarted:
            // Finish is the terminal write, always.
            TaskRegistry::Finish(task, h, BackgroundTaskEvent::State::kCompleted, "done",
                                 false);
        };
        args.handles = captor.handles;
        return registry.Start(std::move(args), error);
    };

    std::string error;
    const auto first = start_blocking(&error);
    REQUIRE(first);
    const auto second = start_blocking(&error);
    CHECK_FALSE(second);
    CHECK(error.find("limit") != std::string::npos);

    release.store(true);
    CHECK(WaitFor([&] { return registry.CountRunning("s1") == 0; }));
    // The refused start consumed no slot; a new one fits again.
    const auto third = start_blocking(&error);
    CHECK(third);
    release.store(true);
    CHECK(WaitFor([&] { return registry.CountRunning("s1") == 0; }));
}

TEST_CASE(
    "TaskRegistry's refusal cleanup does not block other sessions' Start calls",
    "[sidecar][task_registry][regression]") {
    // The bug: Start() ran the refused task's cleanup() while still holding
    // start_mutex_, a process-wide mutex shared by every session's Start()
    // call -- contradicting the function's own documented invariant that the
    // lock covers only the cap check + registration. A slow cleanup (in
    // production, BoundedReapAfterKill can block up to ~5s) wedged every
    // OTHER session's Start behind it. Reproduced here with an artificially
    // slow cleanup instead of a real process kill/reap.
    TaskRegistry registry;
    CapturingHandles captor;

    TaskRegistry::StartArgs capped;
    capped.session_id = "full";
    capped.label = "occupies the only slot";
    capped.id_prefix = "bg";
    capped.max_running = 1;
    std::atomic<bool> release{false};
    capped.body = [&](TaskRegistry::TaskState& task, const TaskRegistry::WatchHandles& h) {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(5ms);
        }
        TaskRegistry::Finish(task, h, BackgroundTaskEvent::State::kCompleted, "done", false);
    };
    capped.handles = captor.handles;
    std::string error;
    REQUIRE(registry.Start(capped, &error));

    // The refused Start's cleanup blocks for a while, simulating
    // BoundedReapAfterKill -- if it runs under start_mutex_, the concurrent
    // Start below (a DIFFERENT session, so it never hits the cap) is stuck
    // behind it too.
    std::atomic<bool> cleanup_started{false};
    TaskRegistry::StartArgs refused;
    refused.session_id = "full";
    refused.label = "over the cap";
    refused.id_prefix = "bg";
    refused.max_running = 1;
    refused.body = [](TaskRegistry::TaskState&, const TaskRegistry::WatchHandles&) {};
    refused.cleanup = [&] {
        cleanup_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(300ms);
    };
    refused.handles = captor.handles;

    std::thread refused_thread([&] {
        std::string refused_error;
        registry.Start(std::move(refused), &refused_error);
    });
    REQUIRE(WaitFor([&] { return cleanup_started.load(std::memory_order_acquire); }));

    // The refused cleanup is now sleeping. A different session's Start must
    // return promptly -- it must not be waiting behind start_mutex_.
    const auto start_time = std::chrono::steady_clock::now();
    TaskRegistry::StartArgs other;
    other.session_id = "other";
    other.label = "unrelated session";
    other.id_prefix = "bg";
    other.body = [&](TaskRegistry::TaskState& task, const TaskRegistry::WatchHandles& h) {
        TaskRegistry::Finish(task, h, BackgroundTaskEvent::State::kCompleted, "done", false);
    };
    other.handles = captor.handles;
    CHECK(registry.Start(std::move(other), &error));
    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    CHECK(elapsed < 100ms);

    release.store(true);
    refused_thread.join();
    CHECK(WaitFor([&] { return registry.CountRunning("full") == 0; }));
}

TEST_CASE("TaskRegistry contains a throwing body: cleanup runs, task fails, event fires",
          "[sidecar][task_registry]") {
    TaskRegistry registry;
    CapturingHandles captor;
    std::atomic<bool> cleaned{false};

    TaskRegistry::StartArgs args;
    args.session_id = "s1";
    args.label = "throwing";
    args.id_prefix = "bg";
    args.body = [&](TaskRegistry::TaskState&, const TaskRegistry::WatchHandles&) -> void {
        throw std::runtime_error("body exploded");
    };
    args.cleanup = [&] { cleaned.store(true); };
    args.handles = captor.handles;

    const auto id = registry.Start(std::move(args), nullptr);
    REQUIRE(id);

    CHECK(WaitFor([&] { return cleaned.load(); }));
    CHECK(WaitFor([&] {
        for (const auto& info : registry.List("s1")) {
            if (info.id == *id) {
                return info.state == BackgroundTaskEvent::State::kFailed;
            }
        }
        return false;
    }));
    const auto events = captor.Events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].second == BackgroundTaskEvent::State::kStarted);
    CHECK(events[1].second == BackgroundTaskEvent::State::kFailed);
}

TEST_CASE("EraseSession drops the records; watchers keep running safely",
          "[sidecar][task_registry]") {
    TaskRegistry registry;
    CapturingHandles captor;
    std::atomic<bool> body_done{false};

    TaskRegistry::StartArgs args;
    args.session_id = "session-a";
    args.label = "work";
    args.id_prefix = "bg";
    args.body = [&](TaskRegistry::TaskState& task, const TaskRegistry::WatchHandles& h) {
        TaskRegistry::Finish(task, h, BackgroundTaskEvent::State::kCompleted, "done", false);
        body_done.store(true);
    };
    args.handles = captor.handles;
    REQUIRE(registry.Start(std::move(args), nullptr));

    CHECK(WaitFor([&] { return body_done.load(); }));
    registry.EraseSession("session-a");
    CHECK(registry.List("session-a").empty());
    CHECK(registry.List("session-b").empty());
}

TEST_CASE("WatchLifetime fires the cleanup when the lifetime expires, exactly once",
          "[sidecar][lifetime_watch]") {
    auto lifetime = std::make_shared<const int>(42);
    std::atomic<int> fired{0};

    // Hold the guard somewhere realistic: the object owning the remote
    // resource holds it.
    std::shared_ptr<void> guard = WatchLifetime(
        lifetime, [&fired] { ++fired; }, 10ms);
    CHECK(fired.load() == 0);

    lifetime.reset();  // the session "ends"
    CHECK(WaitFor([&] { return fired.load() == 1; }, 2s));
    std::this_thread::sleep_for(100ms);  // no double-fire
    CHECK(fired.load() == 1);

    // Destroying the guard after the fire is a no-op.
    guard.reset();
    std::this_thread::sleep_for(50ms);
    CHECK(fired.load() == 1);
}

TEST_CASE("WatchLifetime is disarmed by releasing the guard before expiry",
          "[sidecar][lifetime_watch]") {
    auto lifetime = std::make_shared<const int>(42);
    std::atomic<int> fired{0};
    {
        auto guard = WatchLifetime(lifetime, [&fired] { ++fired; }, 10ms);
        (void)guard;
    }  // explicit teardown before the session ends
    lifetime.reset();
    std::this_thread::sleep_for(200ms);
    CHECK(fired.load() == 0);
}
