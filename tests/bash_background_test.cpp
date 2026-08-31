// Background bash, tier 3: real processes, real pipes, real killpg -- but
// everything the deferred-completion surface needs is a test fake, because the
// surface is exactly three lifetime-safe handles on the ToolContext.
//
// The invariant under test twice over: a task OUTLIVES its invocation. The
// watcher is a detached thread, so every wait here goes through
// tests/support/deadline.hpp discipline -- a poll loop with a deadline; a hang
// reports a failure naming what was waited on, never a wedged suite.

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/tools/tool.hpp"

#include "background_registry.hpp"
#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using tools::BackgroundTaskEvent;
using tools::ToolContext;
using tools::ToolResult;
using tools_bash::BackgroundTaskRegistry;
using tools_bash::BashSessionStateStore;
using tools_bash::BashSettings;

namespace {

// Polls `done` until it flips or the deadline passes. The watcher is detached;
// a deadline without a hang-report is how a regression turns into a five-minute
// suite stall with no name attached.
bool WaitFor(const std::function<bool()>& done,
             std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct Harness {
    TempDir dir;
    BashSessionStateStore store;
    BackgroundTaskRegistry background;
    BashSettings settings;

    // The deferred-completion fakes. `lifetime` is a real control block the
    // test can drop to simulate session death -- the same shape the session's
    // channels block has.
    std::shared_ptr<const void> lifetime = std::make_shared<int>(0);
    std::vector<llm::Message> injected;
    std::mutex injected_mu;
    std::vector<BackgroundTaskEvent> events;
    std::mutex events_mu;
    std::string streamed;

    struct Ack {
        tools::ToolResult result;
        std::string task_id;
        int pid = 0;
    };

    // Runs one background invocation to its acknowledgment. The tool call
    // itself must return immediately -- that is the mode's contract. Set
    // expect_ok=false for a call expected to be REFUSED (the limit).
    Ack Start(const std::string& command, const nlohmann::json& extra = {},
              const std::string& session_id = "s1", bool expect_ok = true) {
        auto tool = tools_bash::MakeBashTool(settings, store, background);
        nlohmann::json input = {{"command", command}, {"run_in_background", true}};
        for (auto it = extra.begin(); it != extra.end(); ++it) input[it.key()] = it.value();

        ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = session_id;
        ctx.project_root = dir.path();
        ctx.tool_use_id = "tu-bg";
        ctx.schedule_injection = [this](llm::Message message) {
            std::lock_guard<std::mutex> lock(injected_mu);
            injected.push_back(std::move(message));
            return true;
        };
        ctx.session_lifetime = lifetime;
        ctx.emit_task_event = [this](const BackgroundTaskEvent& event) {
            std::lock_guard<std::mutex> events_lock(events_mu);
            events.push_back(event);
        };
        ctx.emit_output = [this](std::string_view chunk) { streamed += chunk; };

        const auto started = std::chrono::steady_clock::now();
        const auto result = tool->execute(input, ctx);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        INFO("background ack took " << elapsed << " ms: " << result.content_text);
        CHECK(result.is_error == !expect_ok);
        CHECK(elapsed < 2000);  // the ACK returns, not the command

        Ack ack;
        ack.result = result;
        if (!expect_ok) return ack;
        const auto& text = result.content_text;
        const auto task_marker = text.find("task bg-");
        REQUIRE(task_marker != std::string::npos);
        ack.task_id = text.substr(task_marker + 5,
                                  text.find(' ', task_marker + 5) - (task_marker + 5));
        const auto pid_marker = text.find("(pid ");
        REQUIRE(pid_marker != std::string::npos);
        ack.pid = std::stoi(text.substr(pid_marker + 5));
        return ack;
    }

    std::vector<llm::Message> Injections() {
        std::lock_guard<std::mutex> lock(injected_mu);
        return injected;
    }

    std::vector<BackgroundTaskEvent> Events() {
        std::lock_guard<std::mutex> events_lock(events_mu);
        return events;
    }

    std::optional<BackgroundTaskEvent::State> TaskState(const std::string& id) {
        for (const auto& task : background.List("s1")) {
            if (task.id == id) return task.state;
        }
        return std::nullopt;
    }
};

// The injected message, unpacked: a tool-result user message keyed to the
// original invocation, with (text, is_error).
struct Completion {
    bool present = false;
    bool is_error = false;
    std::string text;
};

Completion SingleCompletion(Harness& h) {
    Completion out;
    auto messages = h.Injections();
    if (messages.size() != 1) return out;
    REQUIRE(messages[0].role == llm::Role::User);
    REQUIRE(messages[0].content.size() == 1);
    const auto* block = std::get_if<llm::ToolResultBlock>(&messages[0].content.front());
    if (block == nullptr) return out;
    CHECK(block->tool_use_id == "tu-bg");  // keyed to the ORIGINAL invocation
    out.present = true;
    out.is_error = block->is_error;
    if (!block->content.empty()) {
        out.text = std::get<llm::TextBlock>(block->content.front()).text;
    }
    return out;
}

bool ReachedTerminal(BackgroundTaskEvent::State state) {
    return state != BackgroundTaskEvent::State::kStarted;
}

}  // namespace

TEST_CASE("a background task acks immediately and injects its result at completion",
          "[tools_bash][background]") {
    Harness h;
    const auto ack = h.Start("echo hello-background");

    // The result lands as a tool-result user message keyed to the original
    // tool_use_id -- the shape the session-naming rule already skips, and the
    // shape a non-blocking subagent's completion uses.
    REQUIRE(WaitFor([&] { return SingleCompletion(h).present; }));
    const auto completion = SingleCompletion(h);
    CHECK_FALSE(completion.is_error);
    CHECK(completion.text.find("hello-background") != std::string::npos);
    CHECK(completion.text.find("[background task " + ack.task_id + " completed]") !=
          std::string::npos);

    // STARTED before the ack returned; terminal state after.
    const auto events = h.Events();
    REQUIRE(events.size() == 2);
    CHECK(events.front().state == BackgroundTaskEvent::State::kStarted);
    CHECK(events.back().state == BackgroundTaskEvent::State::kCompleted);
    CHECK(events.back().task_id == ack.task_id);
    CHECK(events.back().pid == ack.pid);
}

TEST_CASE("a failing background command injects an error result with its status",
          "[tools_bash][background]") {
    Harness h;
    const auto ack = h.Start("echo partial-output; exit 3");

    REQUIRE(WaitFor([&] { return SingleCompletion(h).present; }));
    const auto completion = SingleCompletion(h);
    CHECK(completion.is_error);
    CHECK(completion.text.find("partial-output") != std::string::npos);  // output leads the error
    CHECK(completion.text.find("[exit status: 3]") != std::string::npos);
    CHECK(h.TaskState(ack.task_id) == BackgroundTaskEvent::State::kFailed);
}

TEST_CASE("a background task streams output live and persists only the capped record",
          "[tools_bash][background]") {
    Harness h;
    h.settings.max_output_bytes = 2048;  // head 512 + tail 1536

    const auto ack = h.Start("yes background-stream | head -c 100000");

    REQUIRE(WaitFor([&] { return SingleCompletion(h).present; }));
    // The live channel is uncapped (lossy by design downstream): the watcher
    // pushed far more than the cap through emit_output.
    CHECK(h.streamed.size() > 2048);
    // The injected record is the capped head+tail render, with the omission
    // marker -- identical semantics to a synchronous command's result block.
    const auto completion = SingleCompletion(h);
    CHECK(completion.text.size() < 4096);
    CHECK(completion.text.find("bytes omitted") != std::string::npos);
    CHECK(completion.text.find("background-stream") != std::string::npos);
    (void)ack;
}

TEST_CASE("a background task is killed when the session's lifetime expires",
          "[tools_bash][background]") {
    Harness h;
    // A task that would otherwise run far past every deadline in the suite.
    const auto ack = h.Start("sleep 300");

    CHECK(h.TaskState(ack.task_id) == BackgroundTaskEvent::State::kStarted);

    // Session death: the watcher's lifetime poll is the ONLY kill mechanism --
    // no turn CancelToken is held, because cancelling the turn that started a
    // task must not kill the task. Expiry is bounded by the 100 ms poll slice.
    h.lifetime.reset();
    REQUIRE(WaitFor([&] {
        const auto state = h.TaskState(ack.task_id);
        return state && ReachedTerminal(*state);
    }));
    CHECK(h.TaskState(ack.task_id) == BackgroundTaskEvent::State::kKilled);

    // And the injected result is NOT sent: the session is gone. No injection
    // beyond anything from before the drop, and none will arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(h.Injections().empty());
}

TEST_CASE("a background task honours its wall-clock ceiling and reports the timeout",
          "[tools_bash][background]") {
    Harness h;
    // No timeout supplied: the ceiling IS the budget. Small ceiling, long task.
    h.settings.background_max_timeout_ms = 300;
    const auto ack = h.Start("sleep 30");

    REQUIRE(WaitFor([&] {
        const auto state = h.TaskState(ack.task_id);
        return state && ReachedTerminal(*state);
    }, std::chrono::seconds(10)));
    CHECK(h.TaskState(ack.task_id) == BackgroundTaskEvent::State::kTimedOut);
    const auto completion = SingleCompletion(h);
    REQUIRE(completion.present);
    CHECK(completion.is_error);
    CHECK(completion.text.find("[timed out after 300 ms; process group killed]") !=
          std::string::npos);
    // The process group is actually gone: the pid must not be waitable or
    // killable as our child any more (ECHILD after reap).
    const int rc = killpg(ack.pid, 0);
    CHECK(rc != 0);
    CHECK(errno == ESRCH);
}

TEST_CASE("the per-session background task limit refuses without blocking",
          "[tools_bash][background]") {
    Harness h;
    h.settings.max_background_tasks = 2;

    const auto first = h.Start("sleep 30");
    const auto second = h.Start("sleep 30");
    REQUIRE(h.background.CountRunning("s1") == 2);

    // Refusal, not a wait: the model adapts to the error instead of hanging.
    auto refused = h.Start("sleep 30", {}, "s1", /*expect_ok=*/false);
    CHECK(refused.result.content_text.find("background task limit reached") !=
          std::string::npos);

    // Cleanup: expire the session so the watchers reap their sleepers.
    h.lifetime.reset();
    REQUIRE(WaitFor([&] { return h.background.CountRunning("s1") == 0; },
                    std::chrono::seconds(10)));
    (void)first;
    (void)second;
}

TEST_CASE("a background command never adopts session cwd or environment",
          "[tools_bash][background]") {
    Harness h;
    // The wrapper reports final state on fd 9 -- the watcher drains the
    // channel (a child blocked writing it never exits) but must not adopt it:
    // by the time a background task reports, its cwd/env is stale, and
    // adopting would stomp whatever foreground calls set up in between.
    const auto ack = h.Start("cd /tmp && export MINIMAL_BG_MARKER=1");

    REQUIRE(WaitFor([&] { return SingleCompletion(h).present; }));
    // Compared canonically: the seed is the project root, whatever the OS
    // symlinks (/var -> /private/var on macOS) make it look like. ADOPTED
    // state would be /tmp and the marker env -- that is what must be absent.
    const auto state = h.store.Snapshot("s1", h.dir.path());
    std::error_code ec;
    CHECK(state.cwd == std::filesystem::weakly_canonical(h.dir.path(), ec));
    CHECK(state.env.find("MINIMAL_BG_MARKER") == state.env.end());
    (void)ack;
}

TEST_CASE("background tasks are tracked per session and erased with it",
          "[tools_bash][background]") {
    Harness h;
    const auto ack = h.Start("echo done-session-a", {}, "session-a");
    REQUIRE(WaitFor([&] { return SingleCompletion(h).present; }));

    // Another session's listing is unaffected.
    CHECK(h.background.List("session-b").empty());
    REQUIRE(h.background.List("session-a").size() == 1);
    CHECK(h.background.List("session-a").front().state ==
          BackgroundTaskEvent::State::kCompleted);

    // Teardown drops the records; the finished watcher (already done) had no
    // registry dependency, so this is pure bookkeeping.
    h.background.EraseSession("session-a");
    CHECK(h.background.List("session-a").empty());
    (void)ack;
}
