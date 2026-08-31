// Scripted stdio MCP server for the connection and E2E tests: deterministic,
// no network, argv-driven behaviour. Built as its own executable and spawned
// by tests exactly as a real MCP server would be. Links ONLY cpp-mcp (which
// brings its own bundled json) -- same island rule as mcp_stdio_shim.cpp.
//
// Modes (argv[1]):
//   normal          -- tools: echo{text}, slow{ms}, fail{} (tool error)
//   hang            -- same tools plus hang{} which sleeps 10 minutes
//   die_after_ms:N  -- normal, but the process _exit()s N ms after start
//                      (dies right after a successful initialize)
//   garbage         -- emits non-JSON junk on stdout forever (protocol corrupt)
//
// Everything it logs goes to stderr: stdout is the JSON-RPC pipe.
#include <mcp_message.h>
#include <mcp_server.h>
#include <mcp_tool.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

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

::mcp::json TextContent(const std::string& text) {
    return ::mcp::json::array({::mcp::json{{"type", "text"}, {"text", text}}});
}

void Sleep(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

void RegisterStandardTools(::mcp::server& server, bool with_hang) {
    ::mcp::tool echo = ::mcp::tool_builder("echo")
                           .with_description("Returns the text it was given")
                           .with_string_param("text", "Text to echo")
                           .build();
    server.register_tool(echo, [](const ::mcp::json& args, const std::string&) -> ::mcp::json {
        return TextContent(args.value("text", ""));
    });

    ::mcp::tool slow = ::mcp::tool_builder("slow")
                           .with_description("Sleeps ms milliseconds, then echoes done")
                           .with_number_param("ms", "How long to sleep")
                           .build();
    server.register_tool(slow, [](const ::mcp::json& args, const std::string&) -> ::mcp::json {
        Sleep(std::chrono::milliseconds(static_cast<long long>(args.value("ms", 0.0))));
        return TextContent("done");
    });

    ::mcp::tool fail = ::mcp::tool_builder("fail")
                           .with_description("Always fails")
                           .build();
    server.register_tool(fail, [](const ::mcp::json&, const std::string&) -> ::mcp::json {
        throw ::mcp::mcp_exception(::mcp::error_code::internal_error, "scripted failure");
    });

    ::mcp::tool getenv_tool = ::mcp::tool_builder("getenv")
                                 .with_description("Returns the child process's value for name")
                                 .with_string_param("name", "Environment variable name")
                                 .build();
    server.register_tool(getenv_tool,
                         [](const ::mcp::json& args, const std::string&) -> ::mcp::json {
                             const std::string value = std::getenv(args.value("name", "").c_str()) != nullptr
                                                           ? std::getenv(args.value("name", "").c_str())
                                                           : "";
                             return TextContent(value);
                         });

    if (with_hang) {
        ::mcp::tool hang = ::mcp::tool_builder("hang")
                               .with_description("Sleeps forever")
                               .build();
        server.register_tool(hang, [](const ::mcp::json&, const std::string&) -> ::mcp::json {
            Sleep(std::chrono::minutes(10));
            return TextContent("unreachable");
        });
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";
    StartOrphanWatchdog();
    WritePidFileIfRequested();

    if (mode == "garbage") {
        // Corrupt the protocol channel deliberately: junk on the pipe stdout.
        for (;;) {
            std::printf("THIS IS NOT JSON\n");
            std::fflush(stdout);
            Sleep(std::chrono::seconds(1));
        }
    }

    ::mcp::server::configuration conf;
    conf.name = "fake-mcp-server";
    conf.version = "0.0.1";
    ::mcp::server server(conf);

    if (mode.rfind("die_after_ms:", 0) == 0) {
        RegisterStandardTools(server, false);
        const auto delay = std::chrono::milliseconds(std::stoi(mode.substr(13)));
        std::thread([delay] {
            Sleep(delay);
            std::fflush(stdout);
            std::_Exit(0);  // _exit: no atexit chatter on the JSON-RPC pipe
        }).detach();
    } else if (mode == "hang") {
        RegisterStandardTools(server, true);
    } else {
        RegisterStandardTools(server, false);
    }

    server.start_stdio();
    return 0;
}
