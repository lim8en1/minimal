// `minimal` entry point: TUI by default, headless with -p.
//
// The headless mode is not a TUI flag bolted on -- it is a separate client against
// the same SessionService, and the reason PermissionRunMode::AllowOnly exists.
// Building it first is what proves the proto is a real boundary rather than a
// TUI-shaped hole, and it is the natural end-to-end test harness.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/client/session_client.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"

namespace {

struct HeadlessOptions {
    std::string prompt;
    std::string server;      // gRPC target; empty means the default socket
    std::string agent_preset;
    bool jsonl = false;
    bool bypass = false;
};

// Exit codes are part of the interface: a script piping `minimal -p` needs to
// distinguish "the model finished" from "it was cut off" from "the call failed".
enum ExitCode {
    kOk = 0,
    kTruncated = 2,   // max_tokens / stop_sequence: output is incomplete
    kModelError = 3,  // an ErrorEvent reached us
    kCancelled = 4,
    kUsage = 64,
    kUnavailable = 69,  // no server, or it refused the session
};

void PrintJsonLine(const nlohmann::json& value) {
    std::fputs(value.dump().c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int ExitCodeFor(minimal::v1::StopReason stop_reason) {
    switch (stop_reason) {
        case minimal::v1::STOP_REASON_MAX_TOKENS:
        case minimal::v1::STOP_REASON_STOP_SEQUENCE:
            return kTruncated;
        case minimal::v1::STOP_REASON_CANCELLED:
            return kCancelled;
        case minimal::v1::STOP_REASON_CONTENT_FILTER:
        case minimal::v1::STOP_REASON_REFUSAL:
            return kModelError;
        default:
            return kOk;
    }
}

int RunHeadless(const HeadlessOptions& options) {
    const std::string target =
        options.server.empty()
            ? "unix://" + (minimal::common::DataDir() / "server.sock").string()
            : options.server;

    minimal::client::SessionClient client(target);
    if (!client.WaitForConnected(std::chrono::seconds(5))) {
        std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
        std::fprintf(stderr, "start one with: minimal-server\n");
        return kUnavailable;
    }

    minimal::client::SessionClient::CreateOptions create;
    create.project_root = std::filesystem::current_path().string();
    create.agent_preset = options.agent_preset;
    create.name = options.prompt.substr(0, 60);
    // ALLOW_ONLY unless explicitly overridden. There is no human attached, so an
    // Ask-gated tool must auto-deny rather than block forever on a prompt nobody
    // can answer -- this mode exists for exactly this client.
    create.run_mode = options.bypass ? minimal::v1::PERMISSION_RUN_MODE_BYPASS
                                     : minimal::v1::PERMISSION_RUN_MODE_ALLOW_ONLY;

    const auto created = client.CreateSession(create);
    if (!created.status.ok()) {
        std::fprintf(stderr, "could not create a session: %s\n",
                     created.status.error_message().c_str());
        return kUnavailable;
    }

    int exit_code = kOk;
    bool saw_error = false;

    const auto status = client.RunTurn(
        created.session_id, options.prompt, [&](const minimal::v1::ServerEvent& event) {
            if (options.jsonl) {
                // One JSON value per line, so the stream stays parseable
                // incrementally rather than only once complete.
                nlohmann::json line;
                line["session_id"] = event.session_id();
                if (event.has_stream_event()) {
                    const auto& stream = event.stream_event();
                    if (stream.has_text_delta()) {
                        line["type"] = "text_delta";
                        line["text"] = stream.text_delta().text();
                    } else if (stream.has_message_delta()) {
                        line["type"] = "message_delta";
                        line["stop_reason"] = minimal::v1::StopReason_Name(
                            stream.message_delta().stop_reason());
                    } else {
                        line["type"] = "stream_event";
                    }
                } else if (event.has_tool_execution()) {
                    line["type"] = "tool_execution";
                    line["tool_name"] = event.tool_execution().tool_name();
                    line["started"] = event.tool_execution().started();
                } else if (event.has_error()) {
                    line["type"] = "error";
                    line["message"] = event.error().message();
                } else if (event.has_subagent_started()) {
                    line["type"] = "subagent_started";
                    line["child_session_id"] = event.subagent_started().child_session_id();
                } else {
                    line["type"] = "event";
                }
                PrintJsonLine(line);
            } else if (event.has_stream_event() && event.stream_event().has_text_delta()) {
                // Text mode prints the assistant's words and nothing else.
                std::fputs(event.stream_event().text_delta().text().c_str(), stdout);
                std::fflush(stdout);
            }

            if (event.has_stream_event() && event.stream_event().has_message_delta()) {
                exit_code = ExitCodeFor(event.stream_event().message_delta().stop_reason());
            }
            if (event.has_error()) saw_error = true;

            // Attaching to a child session is a CLIENT PROTOCOL OBLIGATION, not a
            // TUI behaviour: an unattached subagent is killed by the server's
            // initial-attach guard. Subagents are not built yet, so this reports
            // the obligation rather than silently letting the guard become
            // load-bearing.
            if (event.has_subagent_started() && !options.jsonl) {
                std::fprintf(stderr, "\n[subagent %s started]\n",
                             event.subagent_started().child_session_id().c_str());
            }
            return true;
        });

    if (!options.jsonl) std::fputc('\n', stdout);

    if (!status.ok()) {
        std::fprintf(stderr, "stream ended with an error: %s\n", status.error_message().c_str());
        return kUnavailable;
    }
    if (saw_error) return kModelError;
    return exit_code;
}

int RunTui(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // TODO: FTXUI frame -- tab bar / session view / optional panel / status bar,
    // plus a modal layer. See ARCHITECTURE.md -- TUI layout.
    std::puts("minimal: TUI not yet implemented");
    return 0;
}

void PrintUsage() {
    std::puts("usage: minimal [-p PROMPT] [options]\n"
              "\n"
              "  -p, --print PROMPT        run one turn headlessly and exit\n"
              "      --server TARGET       gRPC target (default: unix://<data dir>/server.sock)\n"
              "      --agent PRESET        agent preset (default: settings.json)\n"
              "      --output-format FMT   text (default) or jsonl\n"
              "      --dangerously-bypass  skip every permission check\n"
              "      --version");
}

}  // namespace

int main(int argc, char** argv) {
    HeadlessOptions options;
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(kUsage);
            }
            return argv[++i];
        };

        if (std::strcmp(arg, "--version") == 0) {
            std::printf("minimal %s\n", minimal::common::Version());
            return 0;
        }
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage();
            return 0;
        }
        if (std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--print") == 0) {
            headless = true;
            options.prompt = next("-p");
        } else if (std::strcmp(arg, "--server") == 0) {
            options.server = next("--server");
        } else if (std::strcmp(arg, "--agent") == 0) {
            options.agent_preset = next("--agent");
        } else if (std::strcmp(arg, "--output-format") == 0) {
            const std::string format = next("--output-format");
            if (format == "jsonl") {
                options.jsonl = true;
            } else if (format != "text") {
                std::fprintf(stderr, "unknown --output-format '%s' (want text or jsonl)\n",
                             format.c_str());
                return kUsage;
            }
        } else if (std::strcmp(arg, "--dangerously-bypass") == 0) {
            options.bypass = true;
        }
    }

    if (headless) {
        if (options.prompt.empty()) {
            std::fprintf(stderr, "-p needs a prompt\n");
            return kUsage;
        }
        return RunHeadless(options);
    }
    return RunTui(argc, argv);
}
