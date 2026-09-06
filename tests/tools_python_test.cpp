#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fakes.hpp"
#include "minimal/tools/tool.hpp"
#include "temp_dir.hpp"

#include "python_env.hpp"
#include "python_interpreter.hpp"
#include "python_process.hpp"
#include "python_settings.hpp"
#include "python_store.hpp"
#include "python_tools.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using tools_python::ClassifyDiff;
using tools_python::Interpreter;
using tools_python::InterpreterConfig;
using tools_python::MakeEnvPlan;
using tools_python::ParsePythonSettings;
using tools_python::ParseSlice;
using tools_python::PythonSettings;
using tools_python::PythonSlice;
using tools_python::RequestHandles;
using tools_python::RequestOutcome;
using tools_python::ResolveOnPath;
using tools_python::SpecName;

namespace {

bool HavePython3() { return !ResolveOnPath("python3").empty(); }

// Process-environment scope guard (BuildChildEnv reads the real environ).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_old_ = true;
            old_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};

// A live control block for ctx.session_lifetime -- a default weak_ptr reads as
// "session dead" and the reader would kill the interpreter instantly.
const auto kLifetimeToken = std::make_shared<const int>(1);

void WaitFor(const std::function<bool()>& predicate, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

struct Injected {
    std::vector<llm::Message> messages;
    std::mutex mu;
    // The injection carries a tool-result user message keyed to the original
    // tool_use_id (the background-bash shape): ToolResultBlock wrapping
    // TextBlocks -- look inside it.
    bool Has(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& message : messages) {
            for (const auto& block : message.content) {
                if (std::holds_alternative<llm::TextBlock>(block) &&
                    std::get<llm::TextBlock>(block).text.find(needle) != std::string::npos) {
                    return true;
                }
                if (std::holds_alternative<llm::ToolResultBlock>(block)) {
                    for (const auto& inner : std::get<llm::ToolResultBlock>(block).content) {
                        if (std::holds_alternative<llm::TextBlock>(inner) &&
                            std::get<llm::TextBlock>(inner).text.find(needle) !=
                                std::string::npos) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }
};

// ToolContext the way the agent loop would wire it (lifetime-safe handles
// included -- the reader polls session_lifetime).
struct CtxBuilder {
    tools::ToolContext ctx{common::DefaultLogger()};
    std::shared_ptr<Injected> injected = std::make_shared<Injected>();
    std::vector<tools::BackgroundTaskEvent> events;
    std::shared_ptr<std::mutex> events_mu = std::make_shared<std::mutex>();

    explicit CtxBuilder(const std::filesystem::path& root, const std::string& session = "s1") {
        ctx.session_id = session;
        ctx.project_root = root;
        ctx.tool_use_id = "tu_test";
        ctx.is_cancelled = [] { return false; };
        ctx.cancel_token = std::make_shared<tools::CancelToken>();
        ctx.session_lifetime = kLifetimeToken;
        ctx.schedule_injection = [injected = injected](llm::Message message) {
            std::lock_guard<std::mutex> lock(injected->mu);
            injected->messages.push_back(std::move(message));
            return true;
        };
        ctx.emit_task_event = [events = &events, events_mu = events_mu](
                                  const tools::BackgroundTaskEvent& event) {
            std::lock_guard<std::mutex> lock(*events_mu);
            events->push_back(event);
        };
    }

    void SetSlice(const nlohmann::json* slice) { ctx.tool_config = slice; }

    size_t CompletedEvents() {
        std::lock_guard<std::mutex> lock(*events_mu);
        return std::count_if(events.begin(), events.end(), [](const auto& event) {
            return event.state == tools::BackgroundTaskEvent::State::kCompleted;
        });
    }
};

}  // namespace

// --- pure logic -----------------------------------------------------------------

TEST_CASE("python settings parse with defaults and clamps", "[tools_python][settings]") {
    const auto defaults = ParsePythonSettings(nlohmann::json::object());
    CHECK(defaults.python == "python3");
    CHECK(defaults.sync_wait_ceiling == 300);
    CHECK(defaults.startup_timeout == 10);
    CHECK(defaults.max_output_bytes > 0);

    const auto configured = ParsePythonSettings(nlohmann::json{
        {"python", "python3.12"}, {"sync_wait_ceiling", 5}, {"startup_timeout", 2}});
    CHECK(configured.python == "python3.12");
    CHECK(configured.sync_wait_ceiling == 5);

    // A negative ceiling means "unlimited", the only safe direction for a
    // value whose job is to bound a NON-destructive wait.
    CHECK(ParsePythonSettings(nlohmann::json{{"sync_wait_ceiling", -3}}).sync_wait_ceiling == 0);
    CHECK(ParsePythonSettings(nlohmann::json{{"python", ""}}).python == "python3");
}

TEST_CASE("python slice: null-safe parse of packages and requirements", "[tools_python][slice]") {
    CHECK(ParseSlice(nullptr).empty());
    const nlohmann::json null_json;
    CHECK(ParseSlice(&null_json).empty());

    const nlohmann::json slice_json = nlohmann::json{
        {"packages", nlohmann::json{"impacket", "pefile==2023.2.7"}},
        {"requirements", nlohmann::json{"requirements-dev.txt"}}};
    const auto slice = ParseSlice(&slice_json);
    REQUIRE(slice.packages.size() == 2);
    CHECK(slice.packages[1] == "pefile==2023.2.7");
    REQUIRE(slice.requirements.size() == 1);
    CHECK(slice.requirements[0] == "requirements-dev.txt");
    CHECK_FALSE(slice.empty());
}

TEST_CASE("SpecName normalizes pip distribution names", "[tools_python][diff]") {
    CHECK(SpecName("impacket") == "impacket");
    CHECK(SpecName("pefile==2023.2.7") == "pefile");
    CHECK(SpecName("PyWin32>=306") == "pywin32");
    CHECK(SpecName("some_pkg[extra]==1.0") == "some-pkg");
    CHECK(SpecName("requirements:requirements-dev.txt") == "requirements:requirements-dev.txt");
}

TEST_CASE("ClassifyDiff separates additive entries from spec conflicts",
          "[tools_python][diff]") {
    const std::vector<std::string> running{"impacket", "pefile==2023.2.7",
                                           "requirements:reqs.txt"};
    const auto empty = ClassifyDiff(running, running);
    CHECK(empty.additive.empty());
    CHECK(empty.conflicts.empty());

    // A superset is purely additive: auto-ensure territory.
    const auto additive = ClassifyDiff({running.begin(), running.end() - 1}, running);
    (void)additive;  // subset vs superset direction checked below

    const auto grown = ClassifyDiff(
        std::vector<std::string>{"impacket", "pefile==2023.2.7", "pywin32", "requirements:reqs.txt"},
        running);
    REQUIRE(grown.additive.size() == 1);
    CHECK(grown.additive[0] == "pywin32");
    CHECK(grown.conflicts.empty());

    // The same NAME with a different spec is a conflict, never an install.
    const auto conflicting = ClassifyDiff(std::vector<std::string>{"pefile==2024.1.1"}, running);
    CHECK(conflicting.additive.empty());
    REQUIRE(conflicting.conflicts.size() == 1);
    CHECK(conflicting.conflicts[0].find("pefile==2024.1.1") != std::string::npos);
    CHECK(conflicting.conflicts[0].find("pefile==2023.2.7") != std::string::npos);

    // A requirements path is part of the contract: a different path is
    // additive (installed with -r), not a conflict.
    const auto req_changed = ClassifyDiff(std::vector<std::string>{"requirements:other.txt"},
                                          running);
    REQUIRE(req_changed.additive.size() == 1);
    CHECK(req_changed.conflicts.empty());
}

TEST_CASE("MakeEnvPlan: the declared contract drives venv intent", "[tools_python][env]") {
    TempDir dir;
    const PythonSettings settings;
    const auto bare = MakeEnvPlan(dir.path(), PythonSlice{}, settings);
    CHECK(bare.wants_venv == false);
    CHECK(bare.venv == dir.path() / ".venv");

    const auto declared = MakeEnvPlan(dir.path(), PythonSlice{{"impacket"}, {}}, settings);
    CHECK(declared.wants_venv == true);
    REQUIRE(declared.declared.size() == 1);
    CHECK(declared.declared[0] == "impacket");
}

TEST_CASE("BuildChildEnv inherits PATH, prepends the venv bin, and sets VIRTUAL_ENV",
          "[tools_python][env]") {
    ScopedEnv path("PATH", "/opt/kept/bin");
    TempDir dir;
    const PythonSettings settings;

    // Bare: no declared contract, no venv -- PATH passes through untouched.
    const auto bare = MakeEnvPlan(dir.path(), PythonSlice{}, settings);
    REQUIRE_FALSE(bare.wants_venv);
    const auto bare_env = BuildChildEnv(bare, settings);
    REQUIRE(bare_env.count("PATH") == 1);
    CHECK(bare_env.at("PATH") == "/opt/kept/bin");
    CHECK(bare_env.count("VIRTUAL_ENV") == 0);

    // Venv: bin first, the inherited PATH behind it.
    const auto venv = MakeEnvPlan(dir.path(), PythonSlice{{"requests"}, {}}, settings);
    REQUIRE(venv.wants_venv);
    const auto venv_env = BuildChildEnv(venv, settings);
    CHECK(venv_env.at("PATH").rfind((dir.path() / ".venv" / "bin").string() + ":", 0) == 0);
    CHECK(venv_env.at("PATH").find("/opt/kept/bin") != std::string::npos);
    CHECK(venv_env.at("VIRTUAL_ENV") == (dir.path() / ".venv").string());

    // The driver's capture knob rides along; PYTHONUNBUFFERED is deliberately
    // absent (python -I implies -E and would ignore it anyway).
    CHECK(bare_env.count("_MINIMAL_PY_CAPTURE") == 1);
    CHECK(bare_env.count("PYTHONUNBUFFERED") == 0);
}

// --- interpreter (real python3) --------------------------------------------------

TEST_CASE("the interpreter execs, evals, captures output and keeps state",
          "[tools_python][interpreter]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = std::map<std::string, std::string>{{"PATH", "/usr/bin:/bin:/usr/local/bin"},
                                                    {"HOME", dir.path().string()},
                                                    {"PYTHONUNBUFFERED", "1"},
                                                    {"_MINIMAL_PY_CAPTURE", "1048576"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);
    REQUIRE(error.empty());

    // eval: a trailing expression yields its repr.
    RequestHandles handles;
    handles.tool_use_id = "tu1";
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;
    const auto id1 = interp->Submit("1 + 1", false, handles, &submit_error);
    REQUIRE(id1.has_value());
    RequestOutcome outcome;
    REQUIRE(interp->WaitSync(*id1, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.ok);
    CHECK(outcome.result == "2");

    // exec + persistent globals: state survives across requests.
    const auto id2 = interp->Submit("greeting = 'hello'\n", false, handles, &submit_error);
    REQUIRE(id2.has_value());
    REQUIRE(interp->WaitSync(*id2, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.ok);

    const auto id3 = interp->Submit("greeting + ' world'", false, handles, &submit_error);
    REQUIRE(id3.has_value());
    REQUIRE(interp->WaitSync(*id3, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.result == "'hello world'");

    // stdout is captured per request; a bare call expression yields repr(None).
    const auto id4 = interp->Submit("print('on stdout')", false, handles, &submit_error);
    REQUIRE(id4.has_value());
    REQUIRE(interp->WaitSync(*id4, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.stdout_text == "on stdout\n");
    CHECK(outcome.result == "None");
    CHECK(outcome.ok);
}

TEST_CASE("a trailing expression's value is returned even when statements precede it",
          "[tools_python][interpreter]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    RequestHandles handles;
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;
    RequestOutcome outcome;

    // Statements + a trailing expression: the value survives (exec-only
    // handling of the whole source would return '').
    const auto id1 = interp->Submit("x = 40\nx + 2", false, handles, &submit_error);
    REQUIRE(id1.has_value());
    REQUIRE(interp->WaitSync(*id1, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.ok);
    CHECK(outcome.result == "42");

    // Statement-only code still yields ''.
    const auto id2 = interp->Submit("y = 5", false, handles, &submit_error);
    REQUIRE(id2.has_value());
    REQUIRE(interp->WaitSync(*id2, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.ok);
    CHECK(outcome.result.empty());

    // The trailing expression is evaluated against the state the prefix built.
    const auto id3 =
        interp->Submit("def f():\n    return y\n\nf() * 2", false, handles, &submit_error);
    REQUIRE(id3.has_value());
    REQUIRE(interp->WaitSync(*id3, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.ok);
    CHECK(outcome.result == "10");

    // A syntax error still fails the request honestly and the interpreter lives.
    const auto id4 = interp->Submit("def broken(:", false, handles, &submit_error);
    REQUIRE(id4.has_value());
    REQUIRE(interp->WaitSync(*id4, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.stderr_text.find("SyntaxError") != std::string::npos);
    CHECK(interp->alive());
}

TEST_CASE("a failing request reports the traceback and the interpreter survives",
          "[tools_python][interpreter]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    RequestHandles handles;
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;
    const auto id = interp->Submit("raise ValueError('boom')", false, handles, &submit_error);
    REQUIRE(id.has_value());
    RequestOutcome outcome;
    REQUIRE(interp->WaitSync(*id, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.stderr_text.find("ValueError: boom") != std::string::npos);
    CHECK(interp->alive());

    // The state (and the loop) survived the exception.
    const auto id2 = interp->Submit("'still here'", false, handles, &submit_error);
    REQUIRE(id2.has_value());
    REQUIRE(interp->WaitSync(*id2, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.result == "'still here'");
}

TEST_CASE("input() hits the neutralized stdin instead of eating a protocol frame",
          "[tools_python][interpreter]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    RequestHandles handles;
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;
    const auto id = interp->Submit("input()", false, handles, &submit_error);
    REQUIRE(id.has_value());
    RequestOutcome outcome;
    REQUIRE(interp->WaitSync(*id, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.stderr_text.find("EOFError") != std::string::npos);
    CHECK(interp->alive());

    // The protocol is intact: a later request still routes correctly.
    const auto id2 = interp->Submit("2 * 21", false, handles, &submit_error);
    REQUIRE(id2.has_value());
    REQUIRE(interp->WaitSync(*id2, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.result == "42");
}

TEST_CASE("an async request injects its result keyed to the original tool_use_id",
          "[tools_python][interpreter]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    auto injected = std::make_shared<Injected>();
    std::vector<tools::BackgroundTaskEvent> events;
    auto events_mu = std::make_shared<std::mutex>();
    RequestHandles handles;
    handles.tool_use_id = "tu_async";
    handles.label = "41 + 1";
    handles.session_lifetime = kLifetimeToken;
    handles.schedule_injection = [injected](llm::Message message) {
        std::lock_guard<std::mutex> lock(injected->mu);
        injected->messages.push_back(std::move(message));
        return true;
    };
    handles.emit_task_event = [injected, events_mu](const tools::BackgroundTaskEvent& event) {
        std::lock_guard<std::mutex> lock(*events_mu);
        // unused-injected guard: events go to the local vector below
        (void)event;
    };

    std::string submit_error;
    const auto id = interp->Submit("41 + 1", /*async=*/true, handles, &submit_error);
    REQUIRE(id.has_value());
    WaitFor([&] { return injected->Has("completed"); }, std::chrono::seconds(10));
    CHECK(injected->Has("[python " + *id + " completed]"));
    CHECK(injected->Has("42"));
    CHECK(interp->alive());
}

TEST_CASE("a soft interrupt fails the running request and preserves the interpreter",
          "[tools_python][interpreter][interrupt]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    RequestHandles handles;
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;

    // State first: the whole point of a soft interrupt is keeping it.
    const auto setup = interp->Submit("kept = 'precious'", false, handles, &submit_error);
    REQUIRE(setup.has_value());
    RequestOutcome outcome;
    REQUIRE(interp->WaitSync(*setup, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);

    const auto id = interp->Submit("while True: pass", false, handles, &submit_error);
    REQUIRE(id.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the spin start

    interp->SoftInterrupt();
    const auto landed = interp->WaitForInterrupt(std::chrono::seconds(5), nullptr);
    CHECK(landed == Interpreter::InterruptOutcome::kResponse);
    CHECK(interp->alive());

    // The interrupted request's error outcome arrives; the state written
    // before the spin survives.
    REQUIRE(interp->WaitSync(*id, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.stderr_text.find("KeyboardInterrupt") != std::string::npos);

    const auto check = interp->Submit("kept", false, handles, &submit_error);
    REQUIRE(check.has_value());
    REQUIRE(interp->WaitSync(*check, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.result == "'precious'");
}

TEST_CASE("a soft interrupt with nothing running is reported, not lethal",
          "[tools_python][interpreter][interrupt]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    interp->SoftInterrupt();
    CHECK(interp->WaitForInterrupt(std::chrono::seconds(5), nullptr) ==
          Interpreter::InterruptOutcome::kIdleInterrupt);
    CHECK(interp->alive());

    RequestHandles handles;
    handles.session_lifetime = kLifetimeToken;
    std::string submit_error;
    const auto id = interp->Submit("'alive'", false, handles, &submit_error);
    REQUIRE(id.has_value());
    RequestOutcome outcome;
    REQUIRE(interp->WaitSync(*id, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10),
                             &outcome) == Interpreter::WaitOutcome::kCompleted);
    CHECK(outcome.result == "'alive'");
}

TEST_CASE("consecutive idle interrupts are each reported as idle, never as a response",
          "[tools_python][interpreter][interrupt]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    // The SECOND idle interrupt must read as idle too -- a sticky flag would
    // misreport it as "the running request failed with KeyboardInterrupt".
    for (int round = 0; round < 2; ++round) {
        interp->SoftInterrupt();
        CHECK(interp->WaitForInterrupt(std::chrono::seconds(5), nullptr) ==
              Interpreter::InterruptOutcome::kIdleInterrupt);
        CHECK(interp->alive());
    }
}

TEST_CASE("a hard kill fails pending async requests and the interpreter is replaceable",
          "[tools_python][interpreter][interrupt]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    auto injected = std::make_shared<Injected>();
    RequestHandles handles;
    handles.tool_use_id = "tu_killed";
    handles.session_lifetime = kLifetimeToken;
    handles.schedule_injection = [injected](llm::Message message) {
        std::lock_guard<std::mutex> lock(injected->mu);
        injected->messages.push_back(std::move(message));
        return true;
    };
    std::string submit_error;
    const auto id = interp->Submit("import time; time.sleep(30)", /*async=*/true, handles,
                                   &submit_error);
    REQUIRE(id.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    interp->HardKill("test kill");
    CHECK_FALSE(interp->alive());
    WaitFor([&] { return injected->Has("test kill") || injected->Has("failed"); },
            std::chrono::seconds(10));
    CHECK(injected->Has(*id));
}

TEST_CASE("interpreter death while a sync request is parked delivers exactly ONE result",
          "[tools_python][interpreter][interrupt]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    auto injected = std::make_shared<Injected>();
    RequestHandles handles;
    handles.tool_use_id = "tu_sync_dead";
    handles.session_lifetime = kLifetimeToken;
    handles.schedule_injection = [injected](llm::Message message) {
        std::lock_guard<std::mutex> lock(injected->mu);
        injected->messages.push_back(std::move(message));
        return true;
    };
    std::string submit_error;
    const auto id = interp->Submit("import time; time.sleep(30)", /*sync=*/false, handles,
                                   &submit_error);
    REQUIRE(id.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Park the sync wait, then kill underneath it. The sync request's delivery
    // is its OWN tool result (the woken WaitSync); an injection too would send
    // a second tool_result for the same tool_use_id -- a provider protocol
    // violation.
    int outcome_kind = -1;
    bool outcome_ok = true;
    std::thread waiter([&] {
        RequestOutcome outcome;
        const auto w = interp->WaitSync(*id, nullptr,
                                        std::chrono::steady_clock::now() + std::chrono::seconds(15),
                                        &outcome);
        outcome_kind = static_cast<int>(w);
        outcome_ok = outcome.ok;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    interp->HardKill("test kill");
    waiter.join();

    // The woken wait claims the death outcome; the interpreter is gone.
    CHECK(outcome_kind == static_cast<int>(Interpreter::WaitOutcome::kCompleted));
    CHECK_FALSE(outcome_ok);
    // ...and nothing was injected: the sync request's tool result is the ONLY
    // delivery for its tool_use_id.
    CHECK(injected->messages.empty());
}

TEST_CASE("a sync wait that outlives its deadline DEMOTES and the result still arrives",
          "[tools_python][interpreter][demote]") {
    if (!HavePython3()) SKIP("python3 not available");
    TempDir dir;
    PythonSettings settings;
    settings.startup_timeout = 15;
    InterpreterConfig config;
    config.python_path = ResolveOnPath("python3");
    config.cwd = dir.path();
    config.env = {{"PATH", "/usr/bin:/bin:/usr/local/bin"}, {"HOME", dir.path().string()},
                  {"PYTHONUNBUFFERED", "1"}};
    config.session_lifetime = kLifetimeToken;
    std::string error;
    auto interp = Interpreter::Spawn(config, settings, common::DefaultLogger(), &error);
    REQUIRE(interp != nullptr);

    auto injected = std::make_shared<Injected>();
    RequestHandles handles;
    handles.tool_use_id = "tu_demoted";
    handles.session_lifetime = kLifetimeToken;
    handles.schedule_injection = [injected](llm::Message message) {
        std::lock_guard<std::mutex> lock(injected->mu);
        injected->messages.push_back(std::move(message));
        return true;
    };
    std::string submit_error;
    const auto id = interp->Submit("import time; time.sleep(2); 'slow result'", false, handles,
                                   &submit_error);
    REQUIRE(id.has_value());

    RequestOutcome outcome;  // unfilled on the demote path
    const auto demoted =
        interp->WaitSync(*id, nullptr,
                         std::chrono::steady_clock::now() + std::chrono::milliseconds(500),
                         &outcome);
    CHECK(demoted == Interpreter::WaitOutcome::kDemoted);
    CHECK(interp->alive());

    // The demoted request completes later and is delivered by injection.
    WaitFor([&] { return injected->Has("slow result"); }, std::chrono::seconds(15));
    CHECK(injected->Has("[python " + *id + " completed]"));
    CHECK(interp->alive());
}

// --- the tools -------------------------------------------------------------------

namespace {

struct ToolFixture {
    TempDir dir;
    tools_python::InterpreterStore store;
    PythonSettings settings;

    // Movable (shared_ptr mutex): tests reassign `run = f.Exec(...)` freely.
    struct Run {
        tools::ToolResult result;
        std::shared_ptr<Injected> injected = std::make_shared<Injected>();
        std::vector<tools::BackgroundTaskEvent> events;
        std::shared_ptr<std::mutex> events_mu = std::make_shared<std::mutex>();
    };

    Run Exec(const nlohmann::json& input, const nlohmann::json* slice = nullptr,
             const std::string& session_id = "s1") {
        auto tool = tools_python::MakePythonExecTool(settings, store);
        Run run;
        CtxBuilder builder(dir.path(), session_id);
        builder.SetSlice(slice);
        run.injected = builder.injected;
        run.result = tool->execute(input, builder.ctx);
        {
            std::lock_guard<std::mutex> lock(*builder.events_mu);
            run.events = std::move(builder.events);
        }
        return run;
    }

    Run Exec(CtxBuilder& builder, const nlohmann::json& input,
             const std::string& session_id = "s1") {
        auto tool = tools_python::MakePythonExecTool(settings, store);
        Run run;
        run.injected = builder.injected;
        run.result = tool->execute(input, builder.ctx);
        {
            std::lock_guard<std::mutex> lock(*builder.events_mu);
            run.events = std::move(builder.events);
        }
        return run;
    }

    Run Interrupt(const nlohmann::json& input, const std::string& session_id = "s1") {
        auto tool = tools_python::MakePythonInterruptTool(settings, store);
        Run run;
        CtxBuilder builder(dir.path(), session_id);
        run.injected = builder.injected;
        run.result = tool->execute(input, builder.ctx);
        return run;
    }
};

const nlohmann::json& EmptySlice() {
    static const nlohmann::json json = nlohmann::json::object();
    return json;
}

}  // namespace

TEST_CASE("python_exec starts on first use and state persists across calls",
          "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;

    const auto first = f.Exec(nlohmann::json{{"code", "answer = 40 + 2"}, {"timeout", 10}});
    REQUIRE_FALSE(first.result.is_error);
    CHECK(first.result.content_text.find("py-") != std::string::npos);
    CHECK(first.result.content_text.find("started") != std::string::npos);

    const auto second = f.Exec(nlohmann::json{{"code", "answer"}, {"timeout", 10}});
    REQUIRE_FALSE(second.result.is_error);
    CHECK(second.result.content_text.find("42") != std::string::npos);
}

TEST_CASE("python_exec clean=true restarts into a fresh interpreter",
          "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;

    auto run = f.Exec(nlohmann::json{{"code", "marker = 'old'"}, {"timeout", 10}});
    REQUIRE_FALSE(run.result.is_error);

    run = f.Exec(nlohmann::json{{"code", "marker"}, {"timeout", 10}, {"clean", true}});
    REQUIRE(run.result.is_error);  // NameError: the fresh interpreter knows nothing
    CHECK(run.result.content_text.find("NameError") != std::string::npos);

    // And the fresh interpreter serves normally.
    run = f.Exec(nlohmann::json{{"code", "2 ** 8"}, {"timeout", 10}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("256") != std::string::npos);
}

TEST_CASE("python_exec timeout=0 acknowledges and injects later", "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;
    CtxBuilder builder(f.dir.path());

    const auto run = f.Exec(builder,
                            nlohmann::json{{"code", "import time; time.sleep(1); 'bg done'"}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("started in the background") != std::string::npos);
    CHECK_FALSE(run.events.empty());
    CHECK(run.events.front().state == tools::BackgroundTaskEvent::State::kStarted);

    WaitFor([&] { return run.injected->Has("bg done"); }, std::chrono::seconds(15));
    CHECK(run.injected->Has("completed"));
}

TEST_CASE("python_exec demotes a timed-out sync wait without destroying state",
          "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;
    CtxBuilder builder(f.dir.path());

    auto run = f.Exec(builder, nlohmann::json{{"code", "kept = 'survives'"}, {"timeout", 10}});
    REQUIRE_FALSE(run.result.is_error);

    run = f.Exec(builder,
                 nlohmann::json{{"code", "import time; time.sleep(3); 'late result'"},
                                {"timeout", 1}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("still running") != std::string::npos);

    // The state survives AND the demoted result arrives.
    run = f.Exec(builder, nlohmann::json{{"code", "kept"}, {"timeout", 10}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("survives") != std::string::npos);
    WaitFor([&] { return builder.injected->Has("late result"); }, std::chrono::seconds(15));
}

TEST_CASE("python_interrupt kill=true replaces the interpreter; the next exec starts fresh",
          "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;

    auto run = f.Exec(nlohmann::json{{"code", "state = 'here'"}, {"timeout", 10}});
    REQUIRE_FALSE(run.result.is_error);

    run = f.Interrupt(nlohmann::json{{"kill", true}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("killed") != std::string::npos);

    run = f.Exec(nlohmann::json{{"code", "state"}, {"timeout", 10}});
    REQUIRE(run.result.is_error);  // fresh: NameError
    CHECK(run.result.content_text.find("NameError") != std::string::npos);
}

TEST_CASE("python_interrupt without an interpreter answers instead of crashing",
          "[tools_python][tool]") {
    ToolFixture f;
    const auto run = f.Interrupt(nlohmann::json{{"kill", false}});
    CHECK(run.result.is_error);
    CHECK(run.result.content_text.find("no python interpreter") != std::string::npos);
}

TEST_CASE("python_exec disclosure names the resolved interpreter binary",
          "[tools_python][tool]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;
    auto tool = tools_python::MakePythonExecTool(f.settings, f.store);
    CtxBuilder builder(f.dir.path());
    const auto disclosure = tool->Disclose(nlohmann::json{{"code", "1"}}, builder.ctx);
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(disclosure.grants[0].key.find("python_exec:") == 0);
    CHECK(disclosure.grants[0].label.find("python3") != std::string::npos);
}

TEST_CASE("a bare interpreter drifts onto the venv with a one-time notice, not a restart",
          "[tools_python][tool][env]") {
    if (!HavePython3()) SKIP("python3 not available");
    ToolFixture f;
    CtxBuilder builder(f.dir.path());

    // Start bare: nothing declared, no .venv.
    auto run = f.Exec(builder, nlohmann::json{{"code", "import sys; sys.prefix"}, {"timeout", 15}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("started") != std::string::npos);

    // The project venv appears underneath the live interpreter.
    const std::string create =
        "python3 -m venv '" + (f.dir.path() / ".venv").string() + "'";
    REQUIRE(std::system(create.c_str()) == 0);

    // The next call continues on the (stateful) interpreter and says so, once.
    run = f.Exec(builder, nlohmann::json{{"code", "1 + 1"}, {"timeout", 30}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("project venv now exists") != std::string::npos);

    // One-time: the notice does not repeat.
    run = f.Exec(builder, nlohmann::json{{"code", "2 + 2"}, {"timeout", 15}});
    REQUIRE_FALSE(run.result.is_error);
    CHECK(run.result.content_text.find("project venv now exists") == std::string::npos);
}
