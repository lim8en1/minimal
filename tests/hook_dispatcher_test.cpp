#include <catch2/catch_test_macros.hpp>

#include <csignal>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/agent/hook_observer_bridge.hpp"
#include "minimal/hooks/hook_shell.hpp"

using minimal::hooks::HookDispatcher;
using minimal::hooks::HookPointSpec;
using minimal::hooks::ShellActionOutcome;
using minimal::hooks::ShellActionResult;
using minimal::hooks::RunShellAction;

namespace {

// Mirrors server main()'s process-wide disposition for the duration of a test.
// The disposition is the load-bearing half of the unread-payload fix: the
// poll→write window can never be closed by gating alone.
struct ScopedSignalIgnore {
    ScopedSignalIgnore() { std::signal(SIGPIPE, SIG_IGN); }
    ~ScopedSignalIgnore() { std::signal(SIGPIPE, SIG_DFL); }
};

// A temp dir whose lifetime is the scope; hook scripts and their side-effect
// files live here.
struct TempDir {
    TempDir() {
        const auto base = std::filesystem::temp_directory_path() / "minimal_hook_dispatcher_test";
        std::filesystem::create_directories(base);
        path = base / std::to_string(::getpid());
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }

    std::string Script(const std::string& name, const std::string& body) {
        const auto file = path / name;
        std::ofstream out(file);
        out << body;
        const auto full = file.string();
        ::chmod(full.c_str(), 0755);
        return full;
    }

    std::string Slurp(const std::string& name) const {
        std::ifstream in(path / name);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path path;
};

struct Dispatcher {
    std::unique_ptr<HookDispatcher> dispatcher = std::make_unique<HookDispatcher>(
        minimal::common::DefaultLogger());
};

// Resolves "sh -c" commands through an action config, the same shape hooks.json
// takes after parsing.
minimal::config::HooksConfig OneAction(const std::string& point, const std::string& command,
                                       const std::vector<std::pair<std::string, std::string>>& filter = {},
                                       std::optional<int> timeout_ms = std::nullopt) {
    minimal::config::HookActionConfig action;
    action.on = point;
    action.command = command;
    action.filter = filter;
    action.timeout_ms = timeout_ms;
    minimal::config::HooksConfig config;
    config.default_timeout_ms = 3000;
    config.hooks.push_back(std::move(action));
    return config;
}

}  // namespace

// --- The shell executor ladder -------------------------------------------------

TEST_CASE("exit 0 passes", "[hooks][shell]") {
    const auto result = RunShellAction("exit 0", "{}", std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kPass);
}

TEST_CASE("exit 2 blocks with the first non-empty output line as the reason", "[hooks][shell]") {
    const auto result = RunShellAction("echo ''; echo 'nope: not allowed'; echo other; exit 2",
                                       "{}", std::chrono::milliseconds{3000}, nullptr);
    REQUIRE(result.outcome == ShellActionOutcome::kBlock);
    CHECK(result.block_reason == "nope: not allowed");
}

TEST_CASE("exit 2 with no output blocks with a stated reason", "[hooks][shell]") {
    const auto result = RunShellAction("exit 2", "{}", std::chrono::milliseconds{3000}, nullptr);
    REQUIRE(result.outcome == ShellActionOutcome::kBlock);
    CHECK_FALSE(result.block_reason.empty());
}

TEST_CASE("exit 1 fails open", "[hooks][shell]") {
    const auto result = RunShellAction("echo script broke >&2; exit 1", "{}",
                                       std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
    CHECK(result.fail_reason == "exit status 1");
}

TEST_CASE("a signal death fails open", "[hooks][shell]") {
    const auto result = RunShellAction("kill -9 $$", "{}", std::chrono::milliseconds{3000},
                                       nullptr);
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
}

TEST_CASE("the payload passes through the child's stdin intact", "[hooks][shell]") {
    TempDir dir;
    const auto script = dir.Script("payload.sh", "cat > '" + dir.path.string() +
                                                          "/payload.json'\nexit 0\n");
    const auto result = RunShellAction(script, "{\"tool_name\":\"bash\"}",
                                       std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kPass);
    CHECK(dir.Slurp("payload.json") == "{\"tool_name\":\"bash\"}");
}

TEST_CASE("a hook that exits without reading the payload cannot kill the process",
          "[hooks][shell]") {
    // A notify-style hook (logger, notify-send) exits without reading stdin.
    // The payload is deliberately larger than the 64 KiB pipe buffer, so the
    // parent is guaranteed to still be writing when the read end closes -- the
    // exact window where writing into a dead pipe raised SIGPIPE and, with the
    // default disposition, killed the whole server.
    //
    // Two halves make the fix, and both are needed: the process-wide SIGPIPE
    // ignore (installed in server main; mirrored here, because a poll→write
    // race cannot be closed by gating alone) and the executor's POLLERR/POLLHUP
    // gate, which stops feeding a dead pipe instead of relying on the EPIPE
    // errno path. Before either, this test died with SIGPIPE; now it must
    // complete the action ladder gracefully.
    ScopedSignalIgnore ignore;
    const std::string payload(128 * 1024, 'x');
    const auto result =
        RunShellAction("true", payload, std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kPass);
}

TEST_CASE("a payload larger than the pipe buffer still reaches a reading hook",
          "[hooks][shell]") {
    // The other half of the write gate: stopping on POLLERR/POLLHUP must not
    // stop the POLLOUT path from streaming a payload that takes many writes.
    TempDir dir;
    const auto script =
        dir.Script("drain.sh", "cat > '" + dir.path.string() + "/big.json'\nexit 0\n");
    const std::string payload(200 * 1024, 'p');
    const auto result =
        RunShellAction(script, payload, std::chrono::milliseconds{3000}, nullptr);
    REQUIRE(result.outcome == ShellActionOutcome::kPass);
    CHECK(dir.Slurp("big.json") == payload);
}

TEST_CASE("a hook writing past the filesize backstop fails open", "[hooks][shell]") {
    // A sparse write at an offset beyond the RLIMIT_FSIZE backstop trips the
    // limit immediately -- no gigabytes touch the disk -- while a hook without
    // the backstop would complete and pass. The limit mirrors the bash tool's
    // own; the wall-clock deadline cannot bound disk consumption. The shell
    // reports the signal-killed dd as exit 128+SIGXFSZ; the load-bearing
    // assertion is simply "not a pass", with the file size proving the write
    // was bounded.
    TempDir dir;
    const auto target = (dir.path / "big.bin").string();
    const auto script =
        "dd if=/dev/zero of='" + target + "' bs=1M count=1 seek=1500 2>/dev/null";
    const auto result =
        RunShellAction(script, "{}", std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
    std::error_code ec;
    const auto written = std::filesystem::file_size(target, ec);
    CHECK((ec || written < 1024LL * 1024 * 1024));
}

TEST_CASE("a pipe-open failure degrades to fail-open without touching unrelated fds",
          "[hooks][shell]") {
    // Cap the descriptor budget at exactly the number of fds this process has
    // open, so the executor's first pipe() fails EMFILE. The cleanup path must
    // read only initialized values: POSIX leaves the fd array undefined when
    // pipe() fails, and a garbage close() could take out an unrelated live fd.
    rlimit original{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    struct Restore {
        rlimit saved;
        ~Restore() { ::setrlimit(RLIMIT_NOFILE, &saved); }
    } restore{original};

    std::size_t listed = 0;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) dir = ::opendir("/dev/fd");
    REQUIRE(dir != nullptr);
    while (::readdir(dir) != nullptr) ++listed;  // includes "." and ".."
    ::closedir(dir);
    REQUIRE(listed >= 4);  // ".", "..", and at least two open descriptors

    rlimit capped = original;
    capped.rlim_cur = listed - 2;  // exactly the open fds: the next pipe fails
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &capped) == 0);

    const auto result =
        RunShellAction("exit 0", "{}", std::chrono::milliseconds{3000}, nullptr);
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
    CHECK(result.fail_reason.find("pipe:") != std::string::npos);
}

TEST_CASE("a timed-out action fails open and does not outlive the call", "[hooks][shell]") {
    const auto started = std::chrono::steady_clock::now();
    const auto result = RunShellAction("sleep 30", "{}", std::chrono::milliseconds{200},
                                       nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
    CHECK(result.fail_reason.find("timed out") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds{5});
}

TEST_CASE("a cancellation aborts the wait and fails open", "[hooks][shell]") {
    std::atomic<bool> cancel{false};
    // Flip the flag from another thread; the runner polls every ~50 ms.
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        cancel.store(true);
    });
    const auto result = RunShellAction("sleep 30", "{}", std::chrono::milliseconds{0},
                                       [&cancel] { return cancel.load(); });
    canceller.join();
    CHECK(result.outcome == ShellActionOutcome::kFailOpen);
    CHECK(result.fail_reason == "cancelled");
}

// --- Registration and configuration ---------------------------------------------

TEST_CASE("core points are registered and findable", "[hooks]") {
    Dispatcher d;
    for (const auto* point : {minimal::hooks::kPreToolUse, minimal::hooks::kPostToolUse,
                              minimal::hooks::kUserPromptSubmit, minimal::hooks::kStop,
                              minimal::hooks::kSessionCreated, minimal::hooks::kSessionEnded,
                              minimal::hooks::kPreCompact, minimal::hooks::kPostCompact,
                              minimal::hooks::kSubagentStarted,
                              minimal::hooks::kSubagentCompleted,
                              minimal::hooks::kPermissionPrompted}) {
        CAPTURE(point);
        REQUIRE(d.dispatcher->Find(point) != nullptr);
    }
    // And only those: eleven points, all core-owned.
    REQUIRE(d.dispatcher->Find("core.nonexistent") == nullptr);
}

TEST_CASE("Configure maps actions onto points and validates filters", "[hooks]") {
    Dispatcher d;
    const auto config = OneAction("core.pre_tool_use", "exit 0", {{"tool_name", "bash"}});
    const auto warnings = d.dispatcher->Configure(config);
    CHECK(warnings.empty());

    // Unknown point -> warning, action dropped.
    Dispatcher d2;
    const auto unknown = OneAction("someplugin.nope", "exit 0");
    const auto warnings2 = d2.dispatcher->Configure(unknown);
    REQUIRE(warnings2.size() == 1);
    CHECK(warnings2[0].find("unknown hook point") != std::string::npos);

    // Undeclared filter field -> warning; the point has no "input" filterable.
    Dispatcher d3;
    const auto bad_field = OneAction("core.pre_tool_use", "exit 0", {{"input", "x"}});
    const auto warnings3 = d3.dispatcher->Configure(bad_field);
    REQUIRE(warnings3.size() == 1);
    CHECK(warnings3[0].find("does not declare filterable") != std::string::npos);
}

TEST_CASE("a duplicate point name is recorded twice for the collision check", "[hooks]") {
    Dispatcher d;
    const std::size_t core_attributions = d.dispatcher->attributions().size();  // the 11 core points
    d.dispatcher->SetAttribution("plugin-a");
    (void)d.dispatcher->RegisterPoint({"plugin.point", true, {}});
    d.dispatcher->SetAttribution("plugin-b");
    (void)d.dispatcher->RegisterPoint({"plugin.point", true, {}});

    const auto& attributions = d.dispatcher->attributions();
    REQUIRE(attributions.size() == core_attributions + 2);
    CHECK(attributions[core_attributions].plugin_name == "plugin-a");
    CHECK(attributions[core_attributions + 1].plugin_name == "plugin-b");
    // Both are fed to NameRegistry (NameKind::HookPoint) by main; the FIRST
    // registration stays bound, so nothing silently depends on load order.
}

// --- Dispatch semantics -----------------------------------------------------------

TEST_CASE("a veto-capable emission returns the first block and short-circuits the rest",
          "[hooks]") {
    TempDir dir;
    Dispatcher d;
    auto config = OneAction("core.pre_tool_use",
                            dir.Script("a.sh", "echo first-reason\nexit 2\n"),
                            {{"tool_name", "x"}});
    // A second action whose side effect must NOT happen after the first block.
    minimal::config::HookActionConfig second;
    second.on = "core.pre_tool_use";
    second.filter = {{"tool_name", "x"}};
    second.command =
        dir.Script("b.sh", "touch '" + dir.path.string() + "/should-not-run'\nexit 0\n");
    config.hooks.push_back(std::move(second));
    const auto warnings = d.dispatcher->Configure(config);
    CHECK(warnings.empty());

    const auto blocked = d.dispatcher->Emit("core.pre_tool_use", nlohmann::json{{"tool_name", "x"}});
    REQUIRE(blocked.has_value());
    CHECK(*blocked == "first-reason");
    CHECK(!std::filesystem::exists(dir.path / "should-not-run"));
}

TEST_CASE("filters are exact equality on declared string fields", "[hooks]") {
    Dispatcher d;
    const auto config =
        OneAction("core.pre_tool_use", "exit 2", {{"tool_name", "bash"}}, 1000);
    (void)d.dispatcher->Configure(config);

    CHECK(d.dispatcher->Emit("core.pre_tool_use", nlohmann::json{{"tool_name", "bash"}})
              .has_value());
    CHECK_FALSE(d.dispatcher->Emit("core.pre_tool_use", nlohmann::json{{"tool_name", "bashx"}})
                     .has_value());
    // A non-string or missing field never matches -- "absent" must not mean "any".
    CHECK_FALSE(d.dispatcher->Emit("core.pre_tool_use", nlohmann::json{{"tool_name", 5}})
                     .has_value());
    CHECK_FALSE(d.dispatcher->Emit("core.pre_tool_use", nlohmann::json::object()).has_value());
    // Multiple filter entries are ANDed.
    TempDir dir;
    Dispatcher d2;
    (void)d2.dispatcher->Configure(OneAction(
        "core.pre_tool_use", "exit 2",
        {{"tool_name", "bash"}, {"permission_key", "bash"}}));
    CHECK(d2.dispatcher->Emit("core.pre_tool_use",
                              nlohmann::json{{"tool_name", "bash"}, {"permission_key", "bash"}})
              .has_value());
    CHECK_FALSE(d2.dispatcher
                     ->Emit("core.pre_tool_use",
                            nlohmann::json{{"tool_name", "bash"}, {"permission_key", "read"}})
                     .has_value());
}

TEST_CASE("the payload arrives on the action's stdin with the envelope added", "[hooks]") {
    TempDir dir;
    Dispatcher d;
    (void)d.dispatcher->Configure(OneAction(
        "core.pre_tool_use",
        "cat > '" + dir.path.string() + "/payload.json'\nexit 2\n", {}, 1000));
    (void)d.dispatcher->Emit("core.pre_tool_use", nlohmann::json{{"tool_name", "bash"}});

    const auto payload = nlohmann::json::parse(dir.Slurp("payload.json"));
    CHECK(payload["tool_name"] == "bash");
    CHECK(payload["hook_point"] == "core.pre_tool_use");
    CHECK(payload.contains("timestamp_unix_ms"));
}

TEST_CASE("observe-only points drop a block verdict and report it async", "[hooks]") {
    TempDir dir;
    Dispatcher d;
    (void)d.dispatcher->Configure(OneAction(
        "core.post_tool_use", "echo should-not-block\nexit 2\n", {}, 1000));

    d.dispatcher->EmitAsync("core.post_tool_use", nlohmann::json{{"tool_name", "bash"}});
    d.dispatcher->WaitForIdleForTesting();
    // No crash, no wedged thread; the verdict is discarded by design.
}

TEST_CASE("post_tool_use carries the fields its filters declare", "[hooks][regression]") {
    // THE REGRESSION: core.post_tool_use registered permission_key and
    // category as filterable, but the bridge's payload never carried them --
    // a filter on either could never match.
    TempDir dir;
    Dispatcher d;
    (void)d.dispatcher->Configure(OneAction(
        "core.post_tool_use", "cat > '" + dir.path.string() + "/payload.json'\n",
        {{"category", "execute"}, {"permission_key", "bash"}}, 1000));
    minimal::hooks::HookObserverBridge bridge(*d.dispatcher);

    minimal::agent::ToolInvocation invocation;
    invocation.tool_use_id = "tu1";
    invocation.tool_name = "shell";  // an alias: the filter keys on the permission key
    invocation.permission_key = "bash";
    invocation.category = "execute";
    bridge.OnToolExecuted(minimal::agent::SessionInfo{}, invocation);
    d.dispatcher->WaitForIdleForTesting();

    const auto payload = nlohmann::json::parse(dir.Slurp("payload.json"));
    CHECK(payload["permission_key"] == "bash");
    CHECK(payload["category"] == "execute");
}

TEST_CASE("a disabled handle passes and drops", "[hooks]") {
    minimal::hooks::HookPoint disabled;
    CHECK_FALSE(disabled.enabled());
    CHECK_FALSE(disabled.Emit(nlohmann::json::object()).has_value());
    disabled.EmitAsync(nlohmann::json::object());  // no-op
}
