// Scripted stdio MCP server for the connection and E2E tests: deterministic,
// no network, argv-driven behaviour. Built as its own executable and spawned
// by tests exactly as a real MCP server would be. Hand-rolled newline-
// delimited JSON-RPC, like fake_signal_channel.cpp.
//
// Modes (argv[1]):
//   normal          -- tools: echo{text}, slow{ms}, fail{} (tool error),
//                      getenv{name}
//   hang            -- same tools plus hang{} which sleeps 10 minutes
//   stubborn        -- like hang, but ignores SIGTERM: teardown must escalate
//                      all the way to SIGKILL to reap it
//   die_after_ms:N  -- normal, but the process _exit()s N ms after start
//                      (dies right after a successful initialize)
//   hang_init       -- never answers initialize (reads stdin, replies nothing)
//   garbage         -- emits non-JSON junk on stdout forever (protocol corrupt)
//
// Requests are answered one at a time, in order, on the main thread: the
// harness serialises its calls on one worker anyway, and a tool that sleeps
// simply delays the next answer, as a real single-threaded server would.
//
// Everything it logs goes to stderr: stdout is the JSON-RPC pipe.
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

// When set, the fake server writes its own pid here at startup. Teardown
// tests read it to verify the harness killed the child (the test binary is
// the child's parent, so waitpid observes the death directly).
void WritePidFileIfRequested() {
    const char* path = std::getenv("MINIMAL_FAKE_PID_FILE");
    if (path == nullptr || *path == '\0') return;
    if (FILE* file = std::fopen(path, "w")) {
        std::fprintf(file, "%d\n", static_cast<int>(getpid()));
        std::fclose(file);
    }
}

// ctest waits for EOF on the test binary's stdio pipes, so a fake server that
// outlives its parent (parent killed at timeout, wedged call, orphaned child)
// makes the whole ctest run hang even though the test itself finished. The
// watchdog turns orphanhood into an immediate exit.
void StartOrphanWatchdog() {
    const pid_t original_parent = getppid();
    std::thread([original_parent] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (getppid() != original_parent) std::_Exit(0);
        }
    }).detach();
}

void Emit(const json& message) {
    const std::string line = message.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

json TextResult(const std::string& text, bool is_error = false) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

json ToolListing(bool with_hang) {
    const auto tool = [](const char* name, const char* description, json properties) {
        return json{{"name", name},
                    {"description", description},
                    {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)}}}};
    };
    json tools = json::array({
        tool("echo", "Returns the text it was given",
             {{"text", {{"type", "string"}, {"description", "Text to echo"}}}}),
        tool("slow", "Sleeps ms milliseconds, then echoes done",
             {{"ms", {{"type", "number"}, {"description", "How long to sleep"}}}}),
        tool("fail", "Always fails", json::object()),
        tool("getenv", "Returns the child process's value for name",
             {{"name", {{"type", "string"}, {"description", "Environment variable name"}}}}),
    });
    if (with_hang) tools.push_back(tool("hang", "Sleeps forever", json::object()));
    return {{"tools", tools}};
}

json CallTool(const std::string& name, const json& args) {
    if (name == "echo") return TextResult(args.value("text", ""));
    if (name == "slow") {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<long long>(args.value("ms", 0.0))));
        return TextResult("done");
    }
    if (name == "fail") return TextResult("scripted failure", /*is_error=*/true);
    if (name == "getenv") {
        const char* value = std::getenv(args.value("name", "").c_str());
        return TextResult(value != nullptr ? value : "");
    }
    if (name == "hang") {
        std::this_thread::sleep_for(std::chrono::minutes(10));
        return TextResult("unreachable");
    }
    return TextResult("unknown tool: " + name, /*is_error=*/true);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";
    StartOrphanWatchdog();
    WritePidFileIfRequested();

    if (mode == "hang_init") {
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode == "garbage") {
        // Corrupt the protocol channel deliberately: junk on the pipe stdout.
        for (;;) {
            std::printf("THIS IS NOT JSON\n");
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (mode.rfind("die_after_ms:", 0) == 0) {
        const auto delay = std::chrono::milliseconds(std::stoi(mode.substr(13)));
        std::thread([delay] {
            std::this_thread::sleep_for(delay);
            // SIGKILL, not an exit call: no stdio flush (it can block behind
            // the main thread's stdin read) and no sanitizer exit handling
            // (TSan sleeps a second at exit) -- the death lands on schedule
            // in every build. Every response was already flushed.
            ::kill(::getpid(), SIGKILL);
        }).detach();
    }
    if (mode == "stubborn") std::signal(SIGTERM, SIG_IGN);
    const bool with_hang = mode == "hang" || mode == "stubborn";

    std::string line;
    while (std::getline(std::cin, line)) {
        const json request = json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!request.is_object() || !request.contains("method")) continue;
        if (!request.contains("id") || request.at("id").is_null()) continue;  // a notification
        const std::string method = request.at("method").get<std::string>();
        const json params = request.value("params", json::object());
        json response = {{"jsonrpc", "2.0"}, {"id", request.at("id")}};
        if (method == "initialize") {
            response["result"] = {{"protocolVersion", "2025-03-26"},
                                  {"capabilities", {{"tools", json::object()}}},
                                  {"serverInfo", {{"name", "fake-mcp-server"}, {"version", "0.0.1"}}}};
        } else if (method == "ping") {
            response["result"] = json::object();
        } else if (method == "tools/list") {
            response["result"] = ToolListing(with_hang);
        } else if (method == "tools/call") {
            response["result"] = CallTool(params.value("name", ""),
                                          params.value("arguments", json::object()));
        } else {
            response["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
        }
        Emit(response);
    }
    return 0;
}
