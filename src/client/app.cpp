// `minimal` entry point: TUI by default, headless with -p.
//
// The headless mode is not a TUI flag bolted on -- it is a separate client against
// the same SessionService, and the reason PermissionRunMode::AllowOnly exists.
// Building it first is what proves the proto is a real boundary rather than a
// TUI-shaped hole, and it is the natural end-to-end test harness.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "minimal/client/cli_args.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"

namespace {

using minimal::client::HeadlessOptions;
using minimal::client::TruncateUtf8;

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

// ONE WRITE, UNDER ONE LOCK. This was fputs(dump) followed by fputc('\n') --
// two separate stdio calls, each taking stdout's own lock independently -- and
// it is called from the main thread AND from every subagent-attach thread. So
// another thread's line could land between a JSON body and its newline, which
// breaks the "one JSON value per line" contract this file states outright and
// which is the entire point of the format. The mutex covers the flush too, so a
// partially flushed line cannot interleave either.
void PrintJsonLine(const nlohmann::json& value) {
    static std::mutex mu;
    const std::string line = value.dump() + "\n";
    std::lock_guard<std::mutex> lock(mu);
    std::fwrite(line.data(), 1, line.size(), stdout);
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

// SIGINT/SIGTERM during a turn cancel it through the protocol -- a CancelTurn
// ClientEvent -- instead of killing the process, so the exit code reflects the
// server's own StopReason::Cancelled (exit 4) rather than dying with 130.
//
// The handler itself may only do async-signal-safe work: set a flag, write one
// byte to a self-pipe. The gRPC write happens on the watcher thread, which
// blocks on the pipe and acts on what it reads: 'c' means a signal arrived,
// 'q' is the main thread's wake so the watcher can be joined once the turn has
// ended on its own.
std::atomic<bool> g_cancel_requested{false};
int g_cancel_pipe[2] = {-1, -1};

void CancelSignalHandler(int) {
    g_cancel_requested.store(true, std::memory_order_relaxed);
    const char byte = 'c';
    const ssize_t ignored = write(g_cancel_pipe[1], &byte, 1);
    (void)ignored;
}

void InstallCancelHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = CancelSignalHandler;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void RestoreDefaultHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void CloseCancelPipe() {
    if (g_cancel_pipe[0] != -1) close(g_cancel_pipe[0]);
    if (g_cancel_pipe[1] != -1) close(g_cancel_pipe[1]);
    g_cancel_pipe[0] = g_cancel_pipe[1] = -1;
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
    if (!options.resume_session_id.empty()) {
        // Resume: the stored session keeps its preset and project_root; the
        // server refuses them rather than silently ignoring a conflict.
        create.resume_session_id = options.resume_session_id;
    } else {
        create.project_root = std::filesystem::current_path().string();
        create.agent_preset = options.agent_preset;
        create.name = TruncateUtf8(options.prompt, 60);
    }
    // OUTSIDE the branch, like run_mode: a model the user names is a property of
    // this invocation, not of the stored session -- and the override is in-memory
    // only, so a resumed session has none of its own to keep.
    create.model = options.model;
    // ALLOW_ONLY unless explicitly overridden. There is no human attached, so an
    // Ask-gated tool must auto-deny rather than block forever on a prompt nobody
    // can answer -- this mode exists for exactly this client.
    create.run_mode = options.bypass ? minimal::v1::PERMISSION_RUN_MODE_BYPASS
                                     : minimal::v1::PERMISSION_RUN_MODE_ALLOW_ONLY;

    const auto created = client.CreateSession(create);
    if (!created.status.ok()) {
        std::fprintf(stderr, "%s a session: %s\n",
                     options.resume_session_id.empty() ? "could not create" : "could not resume",
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

    if (options.command) {
        minimal::client::SessionClient::CommandOptions invocation;
        invocation.name = options.command->name;
        invocation.positional = options.command->positional;
        invocation.raw = options.command->raw;
        // No human is attached, so a confirmation cannot be answered -- the same
        // situation AllowOnly exists for on the permission side, and the same
        // answer: decline, and say so, rather than block on a prompt nobody can
        // see. --dangerously-bypass says "assume yes" here too, consistently.
        invocation.on_confirm = [&options](const minimal::v1::ConfirmationRequest& request) {
            if (!options.bypass) return std::string();
            return request.options_size() > 0 ? request.options(0) : std::string();
        };

        const auto command_status = client.RunCommand(
            created.session_id, invocation, [&](const minimal::v1::ServerEvent& event) {
                if (!event.has_command_result()) return true;
                const auto& result = event.command_result();
                if (!result.message().empty()) {
                    std::fputs(result.message().c_str(), stdout);
                    if (result.message().back() != '\n') std::fputc('\n', stdout);
                }
                if (result.status() == minimal::v1::COMMAND_STATUS_ERROR) saw_error = true;
                return true;
            });
        if (!command_status.ok()) {
            std::fprintf(stderr, "stream ended with an error: %s\n",
                         command_status.error_message().c_str());
            return kUnavailable;
        }
        return saw_error ? kModelError : kOk;
    }

    // Wire SIGINT/SIGTERM to a protocol-level cancel before the turn starts.
    // Before this point (connection, CreateSession) the default disposition
    // applies: dying mid-setup is fine, there is no turn to abandon.
    const bool cancel_wired = pipe(g_cancel_pipe) == 0;
    if (!cancel_wired) g_cancel_pipe[0] = g_cancel_pipe[1] = -1;
    std::shared_ptr<minimal::client::SessionClient::TurnHandle> turn_handle;
    std::thread cancel_watcher;
    if (cancel_wired) {
        InstallCancelHandlers();
        cancel_watcher = std::thread([&turn_handle] {
            char byte = 0;
            ssize_t n;
            do {
                n = read(g_cancel_pipe[0], &byte, 1);
            } while (n == -1 && errno == EINTR);
            if (n != 1 || byte != 'c') return;
            // RunTurn populates the handle before its first event; allow a
            // short window for that rather than dropping a cancel that raced
            // the turn's start.
            for (int waited_ms = 0; !turn_handle && waited_ms < 5000; waited_ms += 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (turn_handle) turn_handle->Cancel();
            // Cancelled, or no handle ever arrived (nothing to cancel). Either
            // way a second signal kills instead of deferring.
            RestoreDefaultHandlers();
        });
    }

    const auto status = client.RunTurn(
        created.session_id, options.prompt,
        [&](const minimal::v1::ServerEvent& event) {
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
                } else if (event.has_turn_completed()) {
                    // The child branch had this and the parent's did not, so a
                    // JSONL consumer could see a subagent's turn end but never
                    // the top-level one -- it arrived as {"type":"event"}.
                    line["type"] = "turn_completed";
                    line["hit_max_turns"] = event.turn_completed().hit_max_turns();
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
            // TurnCompletedEvent WINS, and it is the only source that is always
            // present. ARCHITECTURE.md says the headless exit code derives from
            // the turn's stop reason, and §11.4 changed the server specifically
            // so a cancel inside retry backoff reports Cancelled rather than
            // Unknown -- but that path emits no message_delta at all, so reading
            // only the delta left the exit code at 0 and made that fix
            // unobservable from here. This file already asserts that
            // TurnCompletedEvent is the only reliable end-of-turn signal; now it
            // acts on it.
            if (event.has_turn_completed()) {
                exit_code = ExitCodeFor(event.turn_completed().stop_reason());
                // Hitting the turn ceiling is not success: the work stopped
                // short of an answer, which is the same shape as truncation.
                if (event.turn_completed().hit_max_turns()) exit_code = kTruncated;
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
        },
        cancel_wired ? &turn_handle : nullptr);

    if (cancel_wired) {
        // Wake the watcher (it exits without cancelling on 'q') and take the
        // signal disposition back down: past this point there is no turn to
        // abandon, and a late signal should kill, not defer.
        const char wake = 'q';
        const ssize_t ignored = write(g_cancel_pipe[1], &wake, 1);
        (void)ignored;
        cancel_watcher.join();
        RestoreDefaultHandlers();
        CloseCancelPipe();
    }

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
