// Scripted stdio SIGNAL-CHANNEL server for the signal tests: deterministic,
// no threads, no network. Hand-rolled JSON-RPC, like mcp_fake_server.cpp.
//
// Single-threaded on purpose. Every frame the harness will ever receive is
// written from the request loop itself, so nothing ever interleaves with a
// response:
//
//   initialize -> respond with the experimental "minimal/signal" capability,
//                 instructions, and an empty tools capability
//   ping       -> respond {}
//   tools/list -> respond {tools: []}, THEN emit one build.completed signal
//                 notification (params.signal/content/meta per argv mode)
//
// The harness marks a connection alive after tools/list, so the signal lands
// on an idle client -- exactly the state a real channel produces.
//
// Modes (argv[1]):
//   channel         -- declares the capability; emits after tools/list
//   plain           -- declares NO capability (the refusal test)
//
// Optional argv[2]: content for the emitted signal (default "build 123
// passed"). argv[3]: the signal name (default "build.completed").
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

// One full JSON-RPC response or notification per write: a single fwrite of a
// complete line, flushed, from the one thread this process runs.
void Emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// The client's request id, verbatim as written (`"id":` followed by a number
// or a quoted string). The harness matches responses to requests by id, so a
// scripted server must echo the REAL one, whatever the client counts from. Returns "null" when no id is present (a notification).
// Splice a request's id into a response template: the templates hardcode
// "id":1 and this rewrites exactly that first occurrence.
std::string ReplaceId(std::string response, const std::string& id) {
    const auto key = response.find("\"id\":1");
    if (key == std::string::npos || id == "null") return response;
    response.replace(key, 6, "\"id\":" + id);
    return response;
}

std::string ExtractId(const std::string& line) {
    const auto key = line.find("\"id\"");
    if (key == std::string::npos) return "null";
    const auto colon = line.find(':', key);
    if (colon == std::string::npos) return "null";
    const auto value = line.find_first_not_of(" \t", colon + 1);
    if (value == std::string::npos) return "null";
    if (line[value] == '"') {
        const auto close = line.find('"', value + 1);
        if (close == std::string::npos) return "null";
        return line.substr(value, close - value + 1);  // keep the quotes
    }
    const auto end = line.find_first_not_of("0123456789-", value);
    return line.substr(value, end == std::string::npos ? std::string::npos : end - value);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "channel";
    const std::string content = argc > 2 ? argv[2] : "build 123 passed";
    const std::string signal = argc > 3 ? argv[3] : "build.completed";

    const bool declares = mode != "plain";
    // Static, hand-written: a fixture does not need a JSON library to echo
    // three known shapes.
    const std::string initialize_result =
        declares
            ? R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26",)"
              R"("capabilities":{"experimental":{"minimal/signal":{}}},"serverInfo":)"
              R"({"name":"fake-signal-channel","version":"0.0.1"},)"
              R"("instructions":"Build events arrive as <signal source=\"ci\">; run_id names the CI run. Read the logs before acting."}})"
            : R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26",)"
              R"("capabilities":{},"serverInfo":{"name":"fake-signal-channel","version":"0.0.1"}}})";
    const std::string signal_notification =
        R"({"jsonrpc":"2.0","method":"notifications/minimal/signal","params":)"
        R"({"signal":")" + signal +
        R"(","content":")" + content +
        R"(","meta":{"run_id":"123"}}})";

    std::string line;
    bool emitted = false;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const std::string id = ExtractId(line);
        if (line.find("\"initialize\"") != std::string::npos) {
            Emit(ReplaceId(initialize_result, id));
        } else if (line.find("\"ping\"") != std::string::npos) {
            Emit(R"({"jsonrpc":"2.0","id":)" + id + R"(,"result":{}})");
        } else if (line.find("\"tools/list\"") != std::string::npos) {
            Emit(R"({"jsonrpc":"2.0","id":)" + id + R"(,"result":{"tools":[]}})");
            if (declares && !emitted) {
                emitted = true;
                Emit(signal_notification);
            }
        } else if (line.find("\"tools/call\"") != std::string::npos) {
            // A channel offers no tools; refuse loudly rather than pretending.
            Emit(R"({"jsonrpc":"2.0","id":)" + id +
                 R"(,"error":{"code":-32601,"message":"no tools"}})");
        }
        // Anything else (notifications/initialized): read and ignored -- the
        // same leniency a real server needs.
    }
    return 0;
}
