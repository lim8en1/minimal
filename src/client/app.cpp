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
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/client/cli_args.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"

namespace {

using minimal::client::HeadlessOptions;

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
    // Already resolved by ParseArgs -- a bare path became a unix:// target
    // there, so what is printed on failure is what was actually dialled.
    const std::string target =
        options.server.empty() ? minimal::client::DefaultServerTarget() : options.server;

    minimal::client::SessionClient client(target);
    if (!client.WaitForConnected(std::chrono::seconds(5))) {
        std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
        std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                     target.rfind("unix://", 0) == 0 ? target.c_str() + 7 : "<socket path>");
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
    // One attach stream per spawned child. Attaching is a PROTOCOL OBLIGATION:
    // a subagent no client attaches to is terminated by the server's
    // initial-attach guard. Each thread pumps the child's events until the
    // child's single turn completes, then closes the stream (which terminates
    // the child session -- its work is done).
    std::vector<std::thread> child_attaches;

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

            // Attaching to a child session is a CLIENT PROTOCOL OBLIGATION, not
            // a TUI behaviour: an unattached subagent is killed by the server's
            // initial-attach guard. The attach runs on its own thread so the
            // parent's stream keeps pumping; jsonl merges the child's events by
            // session_id, text mode discards them.
            if (event.has_subagent_started()) {
                const std::string child_id = event.subagent_started().child_session_id();
                if (!options.jsonl) {
                    std::fprintf(stderr, "\n[subagent %s started]\n", child_id.c_str());
                }
                child_attaches.emplace_back([&client, child_id, jsonl = options.jsonl] {
                    client.Attach(child_id, [jsonl](const minimal::v1::ServerEvent& child_event) {
                        if (jsonl) {
                            nlohmann::json line;
                            line["session_id"] = child_event.session_id();
                            if (child_event.has_stream_event() &&
                                child_event.stream_event().has_text_delta()) {
                                line["type"] = "text_delta";
                                line["text"] = child_event.stream_event().text_delta().text();
                            } else if (child_event.has_tool_execution()) {
                                line["type"] = "tool_execution";
                                line["tool_name"] = child_event.tool_execution().tool_name();
                                line["started"] = child_event.tool_execution().started();
                            } else if (child_event.has_error()) {
                                line["type"] = "error";
                                line["message"] = child_event.error().message();
                            } else if (child_event.has_turn_completed()) {
                                line["type"] = "turn_completed";
                            } else {
                                line["type"] = "event";
                            }
                            PrintJsonLine(line);
                        }
                        // The child's one turn ends at turn_completed; nothing
                        // follows, and staying attached would hold the session
                        // open. Closing the stream terminates it.
                        return !child_event.has_turn_completed();
                    });
                });
            }
            return true;
        });

    // Wait out every child before reporting: their streams hold the server's
    // refcount, and jsonl consumers expect the children's events too.
    for (auto& thread : child_attaches) {
        if (thread.joinable()) thread.join();
    }

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

}  // namespace

int main(int argc, char** argv) {
    const auto parsed = minimal::client::ParseArgs(argc, argv);
    switch (parsed.action) {
        case minimal::client::ParsedArgs::Action::Version:
            std::printf("minimal %s\n", minimal::common::Version());
            return kOk;
        case minimal::client::ParsedArgs::Action::Usage:
            std::fputs(minimal::client::UsageText().c_str(), stdout);
            return kOk;
        case minimal::client::ParsedArgs::Action::Error:
            std::fprintf(stderr, "%s\n\n", parsed.error.c_str());
            std::fputs(minimal::client::UsageText().c_str(), stderr);
            return kUsage;
        case minimal::client::ParsedArgs::Action::Headless:
            return RunHeadless(parsed.headless);
        case minimal::client::ParsedArgs::Action::Tui:
            break;
    }
    return RunTui(argc, argv);
}
