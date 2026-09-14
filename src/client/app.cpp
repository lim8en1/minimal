// `minimal` entry point: TUI by default, headless with -p.
//
// The headless mode is not a TUI flag bolted on -- it is a separate client against
// the same SessionService, and the reason PermissionRunMode::AllowOnly exists.
// Building it first is what proves the proto is a real boundary rather than a
// TUI-shaped hole, and it is the natural end-to-end test harness.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/keybindings.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/scrollback_refresh.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/frame_renderer.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/commands_help.hpp"
#include "minimal/client/view/keymap_help.hpp"
#include "minimal/client/view/picker_controller.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tab_ui.hpp"
#include "minimal/client/view/theme_picker.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/client/render/color_downsample.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"
#include "minimal/common/model_ref.hpp"

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
    kPaused = 5,      // the session was paused mid-turn (unreachable from -p,
                      // which never sends SetPausedEvent -- mapped so a future
                      // caller cannot mistake suspension for success)
    kUsage = 64,
    kUnavailable = 69,  // no server, or it refused the session
    kInternalError = 70,  // EX_SOFTWARE: an exception escaped somewhere it shouldn't have
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
        case minimal::v1::STOP_REASON_PAUSED:
            return kPaused;
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

// The one JSONL shape for --jsonl mode, shared by the parent turn's own event
// loop and every child/subagent attach loop -- the child loop used to be a
// hand-duplicated subset that had drifted (missing hit_max_turns and
// subagent_started), so a nested subagent's own child spawn, or a child
// hitting the turn ceiling, silently fell through to {"type":"event"}.
nlohmann::json ServerEventToJsonLine(const minimal::v1::ServerEvent& event) {
    nlohmann::json line;
    line["session_id"] = event.session_id();
    if (event.has_stream_event()) {
        const auto& stream = event.stream_event();
        if (stream.has_text_delta()) {
            line["type"] = "text_delta";
            line["text"] = stream.text_delta().text();
        } else if (stream.has_message_delta()) {
            line["type"] = "message_delta";
            line["stop_reason"] =
                minimal::v1::StopReason_Name(stream.message_delta().stop_reason());
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
    } else if (event.has_ask_request()) {
        // The headless client auto-declines these (SessionClient); the JSONL
        // line says what was asked so a script's transcript shows why the
        // model reported an unanswered question.
        line["type"] = "ask_request";
        line["request_id"] = event.ask_request().request_id();
        line["questions"] = event.ask_request().questions_size();
    } else if (event.has_turn_completed()) {
        line["type"] = "turn_completed";
        line["hit_max_turns"] = event.turn_completed().hit_max_turns();
    } else if (event.has_session_ended()) {
        line["type"] = "session_ended";
    } else {
        line["type"] = "event";
    }
    return line;
}

// The common case for OpenTab: a brand-new session rooted at the cwd, no
// resume, no explicit preset/name. Callers that need those (the startup
// resume path, ~line 210) build CreateOptions by hand instead.
minimal::client::SessionClient::CreateOptions DefaultCreateOptions() {
    minimal::client::SessionClient::CreateOptions opts;
    opts.project_root = minimal::client::CurrentProjectRootOrEmpty();
    return opts;
}

int RunHeadless(const HeadlessOptions& options) {
    // Already resolved by ParseArgs -- a bare path became a unix:// target
    // there, so what is printed on failure is what was actually dialled.
    const std::string target =
        options.server.empty() ? minimal::client::DefaultServerTarget() : options.server;

    // Auto-spawn BEFORE the client's channel exists: the probe channel this
    // creates has the process's only sockets at that moment, so the forked
    // server inherits nothing to hold open.
    minimal::client::SpawnOptions spawn_options;
    spawn_options.enabled = options.autospawn;
    const bool reachable = minimal::client::EnsureServerRunning(
        target, spawn_options, [](const std::string& line) {
            std::fprintf(stderr, "minimal: %s\n", line.c_str());
        });

    minimal::client::SessionClient client(target);
    if (!reachable || !client.WaitForConnected(std::chrono::seconds(5))) {
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
        create.project_root = minimal::client::CurrentProjectRootOrEmpty();
        create.agent_preset = options.agent_preset;
        create.name = TruncateUtf8(options.prompt, 60);
    }
    // OUTSIDE the branch, like run_mode: a model named on THIS invocation
    // outranks whatever the stored session was created with. Omitting it (the
    // normal resume) keeps the model the row persisted -- no override here is
    // not "back to the preset default".
    create.model = options.model;
    // ALLOW_ONLY unless explicitly overridden. There is no human attached, so an
    // Ask-gated tool must auto-deny rather than block forever on a prompt nobody
    // can answer -- this mode exists for exactly this client.
    create.run_mode = options.bypass ? minimal::v1::PERMISSION_RUN_MODE_BYPASS
                                     : minimal::v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    create.signals = options.signals;

    // --agent on a RESUME rides the turn, not the create: the stored row's
    // preset governs the session (the server refuses agent_preset on a
    // resume request), but a per-turn override borrows the named preset for
    // exactly this one turn -- which is all a one-shot headless run is.
    minimal::client::SessionClient::TurnOverrides turn_overrides;
    if (!options.resume_session_id.empty() && !options.agent_preset.empty()) {
        turn_overrides.agent_preset = options.agent_preset;
    }

    // SESSIONLESS COMMAND FAST PATH. A command the server catalog marks
    // requires_session=false runs via RunSessionlessCommand with NO session
    // created at all -- the same dispatch path the TUI's sessionless tabs
    // use. A command-only run (`minimal /help`, `/sessions`) that created a
    // session first was the headless twin of the TUI's junk-row bug: the
    // command appends nothing to the transcript, so the run left a live
    // 0-message row behind. An unknown command or one that needs a session
    // falls through to the ordinary create-and-run path below, whose
    // refusals read exactly as they always did.
    if (options.command) {
        const auto listed = client.ListCommands();
        if (listed.status.ok()) {
            for (const auto& info : listed.response.commands()) {
                if (info.name() == options.command->name && !info.requires_session()) {
                    const auto result = client.RunSessionlessCommand(
                        options.command->name, options.command->positional,
                        options.command->raw);
                    if (!result.message().empty()) {
                        std::fputs(result.message().c_str(), stdout);
                        if (result.message().back() != '\n') std::fputc('\n', stdout);
                    }
                    return result.status() == minimal::v1::COMMAND_STATUS_ERROR ? kModelError
                                                                                : kOk;
                }
            }
        }
    }

    const auto created = client.CreateSession(create);
    if (!created.status.ok()) {
        std::fprintf(stderr, "%s a session: %s\n",
                     options.resume_session_id.empty() ? "could not create" : "could not resume",
                     created.status.error_message().c_str());
        return kUnavailable;
    }

    // THE SESSION ID, out where its consumer can find it. Without this the
    // create-then-resume scripting loop (`minimal -p "task"` today, `minimal
    // --resume <id> -p "continue"` tomorrow) had no answer: the id only lived
    // in the database. jsonl puts it IN BAND as the stream's first line (a
    // hand-built object -- it comes from the CreateSession response, not a
    // ServerEvent, so ServerEventToJsonLine never sees it); text mode keeps
    // stdout pure assistant output for piping and prints to stderr instead.
    if (options.jsonl) {
        nlohmann::json line;
        line["type"] = "session_created";
        line["session_id"] = created.session_id;
        PrintJsonLine(line);
    } else {
        std::fprintf(stderr, "[session %s]\n", created.session_id.c_str());
    }

    int exit_code = kOk;
    bool saw_error = false;
    // Set when the server kills the session under us (SessionEndedEvent): the
    // turn never completed, so the run reports as unavailable rather than
    // hanging or claiming success.
    bool session_ended = false;
    // One attach stream per spawned child. Attaching is a PROTOCOL OBLIGATION:
    // a subagent no client attaches to is terminated by the server's
    // initial-attach guard. Each thread pumps the child's events until the
    // child's single turn completes, then closes the stream (which terminates
    // the child session -- its work is done).
    std::vector<std::thread> child_attaches;

    // Shared by the ordinary turn path (RunTurn, below) and a command whose
    // CommandResult carries submitted_turn (right below): both need to print
    // events and derive an exit code the same way, and a queued turn's own
    // output is otherwise indistinguishable from one this process started
    // directly.
    const auto on_turn_event = [&](const minimal::v1::ServerEvent& event) {
        if (options.jsonl) {
            // One JSON value per line, so the stream stays parseable
            // incrementally rather than only once complete.
            PrintJsonLine(ServerEventToJsonLine(event));
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
        // The server-side kill: stop reading and report the run as
        // unavailable. RunTurn's/Attach's read loop treats this event as
        // terminal exactly like turn_completed, so returning false here just
        // makes the stop explicit from the consumer side too.
        if (event.has_session_ended()) {
            session_ended = true;
            return false;
        }

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
                        PrintJsonLine(ServerEventToJsonLine(child_event));
                    }
                    // The child's one turn ends at turn_completed -- or the
                    // child is killed server-side (a cancelled blocking
                    // spawn), which now arrives as session_ended instead of
                    // silence. Nothing follows either signal, and staying
                    // attached would hold the session open. Closing the
                    // stream terminates it.
                    return !(child_event.has_turn_completed() ||
                             child_event.has_session_ended());
                });
            });
        }
        return true;
    };

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

        bool submitted_turn = false;
        const auto command_status = client.RunCommand(
            created.session_id, invocation, [&](const minimal::v1::ServerEvent& event) {
                if (!event.has_command_result()) return true;
                const auto& result = event.command_result();
                if (!result.message().empty()) {
                    std::fputs(result.message().c_str(), stdout);
                    if (result.message().back() != '\n') std::fputc('\n', stdout);
                }
                if (result.status() == minimal::v1::COMMAND_STATUS_ERROR) saw_error = true;
                submitted_turn = result.submitted_turn();
                return true;
            });
        if (!command_status.ok()) {
            std::fprintf(stderr, "stream ended with an error: %s\n",
                         command_status.error_message().c_str());
            return kUnavailable;
        }
        if (!submitted_turn) return saw_error ? kModelError : kOk;

        // A PromptShorthandCommand (e.g. a commands-md /-command that
        // composes a prompt and submits it as a turn): RunCommand's stream
        // already ended at the CommandResult above, but the turn it queued
        // is still running server-side, unobserved -- returning here would
        // report success (or, if the command's own "submitted" message
        // tripped the error check, failure) for work that has not happened
        // yet. Re-attach -- the same protocol obligation a subagent child
        // gets, and the same on_turn_event the ordinary turn path below
        // uses -- so this queued turn's output, exit code and any subagent
        // IT spawns are observed exactly as if `-p` had been given the
        // composed prompt directly.
        //
        // KNOWN LIMITATION: unlike the ordinary turn path, Attach() has no
        // handle for the SIGINT/SIGTERM watcher below (that machinery is
        // wired to RunTurn's TurnHandle, which only a turn STARTED by this
        // process produces) -- a signal here kills the process rather than
        // cancelling the queued turn server-side.
        if (!options.jsonl) std::fputc('\n', stdout);
        const auto attach_status = client.Attach(created.session_id, on_turn_event);
        for (auto& thread : child_attaches) {
            if (thread.joinable()) thread.join();
        }
        if (!options.jsonl) std::fputc('\n', stdout);
        if (!attach_status.ok()) {
            std::fprintf(stderr, "stream ended with an error: %s\n",
                         attach_status.error_message().c_str());
            return kUnavailable;
        }
        if (session_ended) {
            std::fprintf(stderr, "the session was terminated by the server\n");
            return kUnavailable;
        }
        if (saw_error) return kModelError;
        return exit_code;
    }

    // Wire SIGINT/SIGTERM to a protocol-level cancel before the turn starts.
    // Before this point (connection, CreateSession) the default disposition
    // applies: dying mid-setup is fine, there is no turn to abandon.
    const bool cancel_wired = pipe(g_cancel_pipe) == 0;
    if (!cancel_wired) g_cancel_pipe[0] = g_cancel_pipe[1] = -1;
    // Published by the RunTurn thread through on_handle, then flagged
    // release-style: the watcher acquires the flag before reading the
    // shared_ptr. Polling the shared_ptr itself was a data race -- the
    // watcher thread read what the turn thread was writing, with only a
    // sleep as "synchronisation" (TSan).
    std::shared_ptr<minimal::client::SessionClient::TurnHandle> turn_handle;
    std::atomic<bool> turn_handle_ready{false};
    std::thread cancel_watcher;
    if (cancel_wired) {
        InstallCancelHandlers();
        cancel_watcher = std::thread([&turn_handle, &turn_handle_ready] {
            char byte = 0;
            ssize_t n;
            do {
                n = read(g_cancel_pipe[0], &byte, 1);
            } while (n == -1 && errno == EINTR);
            if (n != 1 || byte != 'c') return;
            // RunTurn publishes the handle before its first event; allow a
            // short window for that rather than dropping a cancel that raced
            // the turn's start.
            for (int waited_ms = 0;
                 !turn_handle_ready.load(std::memory_order_acquire) && waited_ms < 5000;
                 waited_ms += 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (turn_handle_ready.load(std::memory_order_acquire)) turn_handle->Cancel();
            // Cancelled, or no handle ever arrived (nothing to cancel). Either
            // way a second signal kills instead of deferring.
            RestoreDefaultHandlers();
        });
    }
    std::function<void(const std::shared_ptr<minimal::client::SessionClient::TurnHandle>&)>
        publish_turn_handle;
    if (cancel_wired) {
        publish_turn_handle = [&turn_handle, &turn_handle_ready](
                                  const std::shared_ptr<
                                      minimal::client::SessionClient::TurnHandle>& handle) {
            turn_handle = handle;
            turn_handle_ready.store(true, std::memory_order_release);
        };
    }

    const auto status = client.RunTurn(created.session_id, options.prompt, on_turn_event,
                                       publish_turn_handle, turn_overrides);

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
    if (session_ended) {
        std::fprintf(stderr, "the session was terminated by the server\n");
        return kUnavailable;
    }
    if (saw_error) return kModelError;
    return exit_code;
}

// --- --list-sessions ---------------------------------------------------------
//
// The headless answer to "which session do I resume?": the picker's data,
// printed. Subagents stay excluded (they belong to their parent's group, like
// the picker's list); the server sorts newest-activity-first already.

int RunListSessions(const minimal::client::HeadlessOptions& options) {
    const std::string target =
        options.server.empty() ? minimal::client::DefaultServerTarget() : options.server;

    // Same autospawn-before-channel discipline as RunHeadless: the probe
    // channel must not hold open descriptors the forked server would inherit.
    minimal::client::SpawnOptions spawn_options;
    spawn_options.enabled = options.autospawn;
    const bool reachable = minimal::client::EnsureServerRunning(
        target, spawn_options, [](const std::string& line) {
            std::fprintf(stderr, "minimal: %s\n", line.c_str());
        });

    minimal::client::SessionClient client(target);
    if (!reachable || !client.WaitForConnected(std::chrono::seconds(5))) {
        std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
        std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                     target.rfind("unix://", 0) == 0 ? target.c_str() + 7 : "<socket path>");
        return kUnavailable;
    }

    std::vector<minimal::v1::SessionSummary> sessions;
    std::optional<int64_t> before_updated_at;
    std::optional<std::string> before_id;
    constexpr int32_t kPageSize = 200;
    for (;;) {
        minimal::client::SessionClient::ListSessionsOptions opts;
        opts.include_all_dirs = options.all_dirs;
        if (!options.all_dirs) opts.project_root = minimal::client::CurrentProjectRootOrEmpty();
        opts.limit = kPageSize;
        opts.before_updated_at = before_updated_at;
        opts.before_id = before_id;
        const auto page = client.ListSessions(opts);
        if (!page.status.ok()) {
            std::fprintf(stderr, "could not list sessions: %s\n",
                         page.status.error_message().c_str());
            return kUnavailable;
        }
        const auto& rows = page.response.sessions();
        for (const auto& row : rows) sessions.push_back(row);
        if (rows.empty() || static_cast<int>(rows.size()) < kPageSize) break;
        before_updated_at = rows[rows.size() - 1].updated_at_unix_ms();
        before_id = rows[rows.size() - 1].session_id();
    }

    if (options.list_json) {
        // ONE array, not NDJSON: a listing is consumed whole
        // (`--list-sessions --output-format json | jq -r '.[0].session_id'`),
        // unlike a turn's jsonl stream which is parsed incrementally by design.
        nlohmann::json out = nlohmann::json::array();
        for (const auto& row : sessions) {
            nlohmann::json item;
            item["session_id"] = row.session_id();
            item["name"] = row.name();
            item["message_count"] = row.message_count();
            item["updated_at_unix_ms"] = row.updated_at_unix_ms();
            item["is_live"] = row.is_live();
            item["project_root"] = row.project_root();
            out.push_back(std::move(item));
        }
        std::fputs(out.dump(2).c_str(), stdout);
        std::fputc('\n', stdout);
        return kOk;
    }

    // Human table. ID first and whole (it is the --resume argument; a ULID is
    // 26 columns, so it gets exactly that), then the quick facts, then the
    // name -- which truncates, being the only column of unbounded width.
    const auto human_time = [](int64_t unix_ms) {
        if (unix_ms <= 0) return std::string("-");
        const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
        std::tm local{};
        localtime_r(&seconds, &local);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
        return std::string(buffer);
    };
    if (sessions.empty()) {
        const std::string root =
            options.all_dirs ? std::string() : minimal::client::CurrentProjectRootOrEmpty();
        if (root.empty()) {
            std::fprintf(stderr, "no stored sessions\n");
        } else {
            std::fprintf(stderr, "no stored sessions for %s\n", root.c_str());
        }
        return kOk;
    }
    for (const auto& row : sessions) {
        // A server-titler-generated name is untrusted display text: a raw
        // ESC or newline in it would either break this table's layout or
        // inject a terminal escape sequence. TruncateUtf8 (not substr) for
        // the same reason options.prompt-derived names already use it at
        // :256 -- a raw byte cut can split a codepoint.
        std::string name =
            row.name().empty() ? "(unnamed)" : minimal::client::StripControlChars(row.name());
        if (name.empty()) name = "(unnamed)";   // a name that was ONLY control chars
        if (name.size() > 48) name = TruncateUtf8(name, 47) + "…";
        std::printf("%-26s  %6d  %s  %s  %s\n", row.session_id().c_str(),
                    row.message_count(), human_time(row.updated_at_unix_ms()).c_str(),
                    row.is_live() ? "live" : "    ", name.c_str());
    }
    return kOk;
}

// --- TUI --------------------------------------------------------------------
//
// Frame assembly (ARCHITECTURE.md "TUI layout"): tab bar / session view /
// optional running-tools panel / status bar, plus a modal layer.
//
// THE BRIDGE: gRPC reader threads never touch FTXUI objects. They mutate
// SessionState/AppState (plain, mutex-guarded state), and a 20 Hz pump posts a
// custom event into the screen loop so it redraws from a snapshot. Every
// component below READS state; nothing on the loop thread waits on the
// reader, and nothing on the reader waits on the loop. Unary RPCs that used
// to run ON the loop thread (RequestScrollback -- née RefreshScrollback,
// SuggestPaths) instead go through AppState's async RPC worker
// (rpc_queue.hpp): Submit() returns immediately, the job runs off-thread and
// publishes into SessionState/AppState the same way a reader thread does.
// That worker is joined explicitly (app->StopRpcQueue(), below, right after
// the pump) rather than left detached, for the same teardown-ordering reason
// the reader/writer threads are joined rather than detached: a job's
// completion touches state that CloseAllTabs() is about to tear down.
//
// Keybindings: every action is dispatched through keybindings.json (the file
// is the config); the table below only maps a key NAME to the byte sequences a
// terminal may send for it. A name the terminal cannot encode matches nothing,
// which is exactly what remapping exists for -- and what the startup warning
// names.

using minimal::client::AppState;
using minimal::client::GrantToggles;
using minimal::client::Keybindings;
using minimal::client::PendingConfirmation;
using minimal::client::PendingPermission;
using minimal::client::PromptEditorState;
using minimal::client::ResolvedKeymap;
using minimal::client::SessionState;
using minimal::client::TabUi;
using minimal::client::SessionStateKind;
using minimal::client::Theme;

// What a quit / tab-close confirmation will act on, when answered: the
// confirm modal chooses TERMINATE (destroy the sessions and everything in
// flight) or DETACH (end only the streams; work finishes unattended and
// /sessions reattaches).
struct PendingClose {
    bool quit = false;
    // STAGING ORDER: render order -- the root FIRST, its children directly
    // after (TabsToClose walks the tab list, where children follow their
    // parent). Closing the root terminates the whole group, so the child
    // closes below land on already-terminated sessions and are no-ops; the
    // order is therefore safe either way, but these comments used to claim
    // children-first, which is not what the staging produces.
    std::vector<std::string> session_ids;
    bool any = false;
};

int RunTui(const minimal::client::ParsedArgs& args) {
    using namespace ftxui;
    namespace plugins = minimal::client::plugins;

    const std::string target = args.headless.server.empty()
                                   ? minimal::client::DefaultServerTarget()
                                   : args.headless.server;

    // Auto-spawn before the client's channel exists, so the forked server
    // inherits no stray descriptors (same reason as the headless path).
    minimal::client::SpawnOptions spawn_options;
    spawn_options.enabled = args.headless.autospawn;
    const bool reachable =
        minimal::client::EnsureServerRunning(target, spawn_options, [](const std::string& line) {
            std::fprintf(stderr, "minimal: %s\n", line.c_str());
        });

    auto client = std::make_shared<minimal::client::SessionClient>(target);
    if (!reachable || !client->WaitForConnected(std::chrono::seconds(2))) {
        std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
        std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                     target.rfind("unix://", 0) == 0 ? target.c_str() + 7 : "<socket path>");
        return kUnavailable;
    }

    // The startup --resume probe: resolve the target session BEFORE the UI is
    // built, so a bad id fails fast with a clear line instead of an empty TUI.
    // What is kept from the summary is exactly what the attach step needs:
    // liveness (attach vs revive) and the subagent refusal (a subagent's life
    // is one turn of its parent; opening one standalone has no meaning -- the
    // server refuses its resume too).
    std::optional<minimal::v1::SessionSummary> startup_resume;
    if (!args.headless.resume_session_id.empty()) {
        const auto found = client->GetSession(args.headless.resume_session_id);
        if (found.status.error_code() == grpc::StatusCode::NOT_FOUND) {
            std::fprintf(stderr, "no such session: %s\n",
                         args.headless.resume_session_id.c_str());
            std::fprintf(stderr, "list stored sessions with: minimal --list-sessions\n");
            return kUnavailable;
        }
        if (!found.status.ok()) {
            std::fprintf(stderr, "could not look up session %s: %s\n",
                         args.headless.resume_session_id.c_str(),
                         found.status.error_message().c_str());
            return kUnavailable;
        }
        if (!found.summary.parent_session_id().empty()) {
            std::fprintf(stderr, "session %s is a subagent of %s and cannot be opened "
                                 "standalone; open the parent session instead\n",
                         args.headless.resume_session_id.c_str(),
                         found.summary.parent_session_id().c_str());
            return kUnavailable;
        }
        startup_resume = found.summary;
    }

    // The /-command catalog, fetched ONCE at connect (plugins load at startup
    // and config is read once, so the GLOBAL list cannot change mid-connection
    // -- no invalidation machinery) and merged server-side with the STARTUP
    // project root's own commands-md discoveries, if any (ProjectCommandSource
    // -- a project command may shadow a global one of the same name). Feeds
    // the prompt editor's /-autocomplete and the /help overlay.
    //
    // SIMPLIFICATION, stated precisely: this is the startup root only. A tab
    // opened later against a DIFFERENT project root (a fresh `/new` elsewhere
    // on disk) still DISPATCHES that root's own project commands correctly --
    // Session::ExecuteCommand resolves against the session's own project_root,
    // never this catalog -- it just will not autocomplete-discover them here.
    // A full fix needs a per-tab, per-focus-change catalog refresh; this
    // covers the overwhelmingly common case (one project per invocation) at a
    // fraction of the machinery.
    std::vector<minimal::client::CommandCatalogEntry> command_catalog;
    bool commands_listed = false;
    {
        const auto listed = client->ListCommands(minimal::client::CurrentProjectRootOrEmpty());
        if (listed.status.ok()) {
            commands_listed = true;
            for (const auto& info : listed.response.commands()) {
                command_catalog.push_back(
                    {info.name(), info.summary(), info.read_only(),
                     info.kind() == minimal::v1::COMMAND_KIND_PROMPT_SHORTHAND,
                     info.requires_session()});
            }
        }
    }
    // /new, /detach-session, /rewind, /redo are intercepted client-side
    // (submit_command, below) rather than routed through the server's
    // ListCommands/Command dispatch, so they have no CommandInfo of their own
    // to appear here -- added by hand so /-autocomplete still offers them.
    // requires_session: /new opens a sessionless TAB (no session to
    // require); /rewind and /redo act on a session's transcript, and
    // /detach-session detaches the focused tab's session group, so all three
    // are hidden until one exists. /keymap joins them (client-side data,
    // nothing to require).
    command_catalog.push_back({"new", "start a new session", /*read_only=*/false,
                               /*submits_prompt=*/false, /*requires_session=*/false});
    command_catalog.push_back({"keymap", "show all configured key bindings",
                               /*read_only=*/true,
                               /*submits_prompt=*/false, /*requires_session=*/false});
    command_catalog.push_back({"rewind", "undo the most recent user turn (or N of them)",
                               /*read_only=*/false, /*submits_prompt=*/false,
                               /*requires_session=*/true});
    command_catalog.push_back({"redo", "restore the most recently rewound turn",
                               /*read_only=*/false, /*submits_prompt=*/false,
                               /*requires_session=*/true});
    command_catalog.push_back({"detach-session",
                               "detach this tab group; work keeps running, /sessions reattaches",
                               /*read_only=*/false, /*submits_prompt=*/false,
                               /*requires_session=*/true});
    command_catalog.push_back({"pause",
                               "pause the agent (stops after its current tool call)",
                               /*read_only=*/false, /*submits_prompt=*/false,
                               /*requires_session=*/true});
    command_catalog.push_back({"resume",
                               "resume a paused session (continues from its last tool result)",
                               /*read_only=*/false, /*submits_prompt=*/false,
                               /*requires_session=*/true});
    // /theme is intercepted client-side like /new (it swaps the in-memory
    // Theme and re-pushes it to plugins; the server has no part in it), so it
    // has no CommandInfo -- added by hand for /-autocomplete. Works on a
    // sessionless tab: decoration, not session state.
    command_catalog.push_back({"theme", "switch the UI theme",
                               /*read_only=*/true, /*submits_prompt=*/false,
                               /*requires_session=*/false});

    // --- config + client plugins --------------------------------------------
    // ORDER MATTERS -- see tui_bootstrap.hpp/.cpp for the full sequence
    // (ui.json -> grammars -> plugin load -> collisions -> theme ->
    // keybindings -> status format -> highlighter/renderer/engine -> chat
    // surface). `env` MUST be a plain local, never moved/copied/returned:
    // several of its fields hold pointers into ITS OWN other fields (the
    // header's address-stability contract), so BuildTuiEnvironment fills it
    // in place rather than returning one by value.
    minimal::client::TuiEnvironment env;
    if (!minimal::client::BuildTuiEnvironment(*client, env)) return kUnavailable;

    // Local aliases, same names the inline code used to declare directly:
    // every lambda below this point captures `[&]`, i.e. by ordinary lexical
    // scoping, so introducing these here (rather than rewriting every
    // capture site to say `env.theme` etc.) is what keeps the rest of this
    // function's text unchanged. `theme`/`engine` deref an optional/pointer
    // that BuildTuiEnvironment guarantees non-null on success.
    Theme& theme = *env.theme;
    plugins::MarkdownEngine& engine = *env.engine;
    plugins::ChatSurface* chat_surface = env.chat_surface;
    auto& view_registry = env.view_registry;
    auto& start_view_registry = env.start_view_registry;
    auto& renderer_registry = env.renderer_registry;
    auto& display_registry = env.display_registry;
    auto& message_registry = env.message_registry;
    auto& status_registry = env.status_registry;
    const auto& status_format = env.status_format;
    const auto& keymap = env.keymap;
    auto& plugin_views = env.plugin_views;

    auto app = std::make_shared<AppState>(client);
    app->SetPluginAttention(&env.attention_state);
    app->SetChatSurface(chat_surface);
    // THE STARTUP RESUME, on the probed session (probe above, before the UI
    // was built). LIVE opens the whole tab group -- the session plus every
    // live subagent child (AttachSessionGroup); anything else REVIVES the
    // stored transcript into a fresh root group (ResumeTab), the picker's own
    // stale-liveness fallthrough. "" from the live path means the session
    // ended between probe and attach; resume is then the remaining option,
    // and its failure is visible as an Error tab, never silent. Either way
    // the tab focuses itself (focus-if-empty), so no separate SetFocusedTab
    // is needed here.
    if (startup_resume) {
        const std::string session_id = startup_resume->session_id();
        if (startup_resume->is_live()) {
            if (!app->AttachSessionGroup(session_id).empty()) startup_resume = std::nullopt;
        }
        if (startup_resume) (void)app->ResumeTab(session_id);
    }
    // The focus seam's target, now that AppState exists (host_seams.hpp:
    // plugin load ran before app construction, so the provider attaches here).
    // Plugins captured the provider POINTER at registration; anything they
    // read through it happens later, so the attach is never raced.
    env.host_focus_provider->Attach(app.get());
    // Memory bounds (track R2): ui.json's values, or the built-in defaults
    // (SetUiBounds treats 0 as "keep the default"). Applied before any tab can
    // open, so every SessionState starts bounded.
    app->SetUiBounds(env.ui.scrollback_window_max_messages, env.ui.tool_output_buffer_bytes);
    // FULLSCREEN ALTERNATE SCREEN, not TerminalOutput: the alternate buffer is
    // repaint-safe on terminal resize. TerminalOutput repaints the PRIMARY
    // screen by cursor arithmetic over the previous frame, and a resize reflows
    // the text ftxui still thinks it owns -- every repaint then re-stamps stale
    // frame fragments (old tab-bar lines, typed text) as "junk" above the live
    // frame. Any out-of-band stdout/stderr write would desync it the same way.
    auto screen = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    // ftxui 7 force-handles Ctrl+C by default -- it exits the loop and
    // re-raises SIGINT EVEN when a component consumes the event -- which
    // defeats this client's busy-cancel and quit confirmation. The client's
    // own dispatch (keybinding_dispatch.cpp) must see Ctrl+C first.
    screen.ForceHandleCtrlC(false);
    auto quit = screen.ExitLoopClosure();

    // --- frame state (loop thread only) --------------------------------------
    PromptEditorState prompt_state;
    bool show_panel = false;
    bool show_picker = false;
    bool show_agent_picker = false;
    bool show_model_picker = false;
    bool show_mcp_picker = false;
    bool show_theme_picker = false;
    bool show_keymap_help = false;
    bool show_commands_help = false;
    bool show_quit_confirm = false;
    bool show_permission = false;
    bool show_confirmation = false;
    bool show_delete_confirm = false;
    bool show_plugin_modal = false;
    std::string staged_delete_id;   // the picker delete confirmation's subject
    PendingClose pending_close;

    // Per-tab UI caches, keyed by TAB id (AppState's tab handle -- the only
    // key that exists for a sessionless tab). TabUi's type and the
    // maintenance logic that reads/writes it both live in
    // view/tab_ui.{hpp,cpp}; the MAP stays a plain RunTui local (not owned
    // by TabMaintenance) because close_tab (below) erases from it directly,
    // and close_tab is defined before TabMaintenance can be constructed --
    // it needs open_picker/plugin_modal_child, built later.
    std::map<std::string, minimal::client::TabUi> tab_ui;

    // Tab close, for every caller (keybinding, quit confirmation, quit,
    // /detach-session): the per-tab UI caches must die WITH the tab --
    // tab_ui entries were never erased, leaking each closed tab's streaming
    // buffer, grants and surface body for the process lifetime. Defined
    // HERE (before submit_command) because /detach-session intercepts the
    // command client-side and erases the group's tabs through it.
    auto close_tab = [&](const std::string& id) {
        app->CloseTab(id);
        tab_ui.erase(id);
    };

    // Picker-open lambdas, assigned after PickerController exists (below):
    // submit_command (above's definition point) intercepts the bare
    // /sessions //agent //model forms on sessionless tabs and needs to reach
    // these without a server round trip.
    std::function<void()> open_picker_fn = [] {};
    std::function<void()> open_agent_picker_fn = [] {};
    std::function<void()> open_model_picker_fn = [] {};
    std::function<void()> open_subagent_model_picker_fn = [] {};
    std::function<void()> open_mcp_picker_fn = [] {};
    std::function<void()> open_theme_picker_fn = [] {};

    // Keymap-help overlay opener, assigned after the overlay exists (below):
    // submit_command intercepts bare /keymap the same way it intercepts bare
    // /help -- the overlay is where the full binding listing lives.
    std::function<void()> open_keymap_help_fn = [] {};

    // `focused`/`focused_details` become aliases into TabMaintenance once it
    // exists (below, after open_picker/plugin_modal_child) -- see the
    // TabMaintenance construction for why it cannot be built here yet.

    // --- editor ---------------------------------------------------------------
    //
    // T2: @-mention suggestions used to run SuggestPaths synchronously here,
    // on the loop thread, from inside the prompt editor's own Renderer --
    // a merely slow server stalled every keystroke after '@' for up to the
    // RPC's 250 ms deadline. This lambda's contract (prompt_editor.hpp's
    // MentionSuggestor) is now "nullopt = still waiting, poll again next
    // frame": the FIRST call for a given (session, prefix) submits the
    // request onto AppState's async RPC worker and returns nullopt; later
    // calls for the SAME (session, prefix) keep returning nullopt until the
    // worker's completion callback (below) records the answer, at which
    // point this call consumes and returns it. A prefix change supersedes
    // whatever was in flight -- the completion for the OLD request still
    // arrives eventually, but by then `query` no longer names it, so it is
    // dropped rather than clobbering the newer one (see the guard inside the
    // callback).
    struct MentionQuery {
        std::mutex mtx;
        // The query scope, "session:<id>" or "root:<path>" -- keyed by scope
        // rather than raw session id so a tab that mints its session
        // mid-query (sessionless -> sessioned) supersedes its own root-scoped
        // answer instead of consuming it under a changed meaning.
        std::string scope;
        std::string prefix;
        bool in_flight = false;
        bool ready = false;
        std::vector<std::string> result;
    };
    auto mention_query = std::make_shared<MentionQuery>();
    auto suggest = [app, mention_query](
                       const std::string& prefix) -> std::optional<std::vector<std::string>> {
        const std::string sid = app->FocusedSessionId();
        if (prefix.empty()) return std::vector<std::string>{};

        // Sessioned tabs query by session; a SESSIONLESS tab owns no session
        // row yet (one is minted on its first message), so its query scopes
        // to the project root that first message's session will be created
        // with (DefaultCreateOptions) -- the suggestions then match what the
        // eventual session would see, instead of the sid.empty() guard
        // silently killing '@' on every fresh tab. No project context at all
        // (a stray cwd failure) still answers empty.
        std::string scope;
        bool session_scoped = false;
        std::string root;
        if (!sid.empty()) {
            scope = "session:" + sid;
            session_scoped = true;
        } else {
            root = minimal::client::CurrentProjectRootOrEmpty();
            if (root.empty()) return std::vector<std::string>{};
            scope = "root:" + root;
        }

        std::lock_guard lk(mention_query->mtx);
        if (mention_query->scope == scope && mention_query->prefix == prefix) {
            if (mention_query->ready) {
                mention_query->in_flight = false;
                mention_query->ready = false;
                return std::move(mention_query->result);
            }
            if (mention_query->in_flight) return std::nullopt;   // still waiting
        }

        // A new (scope, prefix) pair: submit it, superseding whatever this
        // slot previously tracked (its completion, if it arrives late, will
        // no longer match scope/prefix and will be dropped -- see
        // RequestSuggestPaths's callback below).
        mention_query->scope = scope;
        mention_query->prefix = prefix;
        mention_query->in_flight = true;
        mention_query->ready = false;
        if (session_scoped) {
            app->RequestSuggestPaths(sid, prefix, 20, [mention_query, scope, prefix](
                                                          std::vector<std::string> paths) {
                std::lock_guard cb_lk(mention_query->mtx);
                if (mention_query->scope != scope || mention_query->prefix != prefix) {
                    return;   // superseded by a newer request; drop the stale answer
                }
                mention_query->result = std::move(paths);
                mention_query->ready = true;
            });
        } else {
            app->RequestSuggestPathsForRoot(root, prefix, 20, [mention_query, scope, prefix](
                                                                  std::vector<std::string> paths) {
                std::lock_guard cb_lk(mention_query->mtx);
                if (mention_query->scope != scope || mention_query->prefix != prefix) {
                    return;   // superseded by a newer request; drop the stale answer
                }
                mention_query->result = std::move(paths);
                mention_query->ready = true;
            });
        }
        return std::nullopt;
    };
    // --- tabs are sessionless until the first message ---------------------------
    // OpenTab mints a TAB (a client-side container, no RPC, no server row);
    // the session comes into existence only when a message is submitted
    // (SubmitChatTurn -> AppState::EnsureSession, single-flight per tab on the
    // RPC worker). The old create_session_and_run queue-and-create machinery
    // lives inside AppState now, per tab; RunTui just opens-or-reuses a tab
    // and hands it the action. This is the structural fix for the 0-message
    // junk sessions: nothing reaches the server until there is something to
    // say, and a tab abandoned before that leaves no row at all.
    auto focused_or_new_tab = [app]() -> std::string {
        if (std::string tab = app->FocusedTabId(); !tab.empty()) return tab;
        return app->OpenTab(DefaultCreateOptions());   // local-only; focuses itself
    };
    auto submit_chat = [app, &tab_ui, focused_or_new_tab](const std::string& text) {
        const std::string tab = focused_or_new_tab();
        // Sending a turn returns the view to the newest row: scrolled-up
        // content would otherwise sit frozen while the reply streams in
        // somewhere below the visible viewport, reading as if nothing
        // happened. Same for a row cursor left over from browsing -- it would
        // otherwise keep pinning the view to a now-stale row through the
        // whole new turn. (A sessionless tab has no chat view yet -- the map
        // lookup misses -- and nothing to scroll.)
        if (auto it = tab_ui.find(tab); it != tab_ui.end() && it->second.chat) {
            it->second.chat->ScrollToBottom();
            it->second.chat->ClearRowFocus();
        }
        app->SubmitChatTurn(tab, text);
    };
    // --- theme switching -------------------------------------------------------
    // THE SWAP PATH, shared by "/theme <name>" and the picker's Enter: replace
    // `theme`'s CONTENTS in place (the object keeps its address -- every view,
    // the syntax highlighter and the markdown renderer hold references/pointers
    // to it and re-read ColorFor per frame, so the next paint is already the new
    // palette), drop the highlighter's color-baked cache, re-push OnTheme to the
    // theme-aware plugins, and record the choice in ui.json for the next launch.
    // A failed load changes nothing and reports the error through the tab
    // notice -- a typo'd name must not blank the UI.
    auto apply_theme = [app, &env, &theme](const std::string& name) {
        Theme loaded;
        try {
            loaded = Theme::Load(env.config_dir / "themes", name, env.depth);
        } catch (const std::exception& error) {
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->TabNotice(tab, "theme: " + std::string(error.what()));
            }
            return;
        }
        theme = std::move(loaded);
        env.highlighter->OnThemeChanged();
        // PushThemeToPlugins disables a throwing chat surface only in `env`
        // (correct for the FIRST call, during BuildTuiEnvironment, before
        // AppState exists -- RunTui reads env.chat_surface into AppState
        // right after). This is a LATER call, after AppState already has its
        // own cached pointer and sticky broken-flag (EffectiveChatSurface) --
        // without this sync, a surface that throws on a SWITCH (not the
        // first load) stays the effective surface, still receiving events.
        const bool surface_was_live = env.chat_surface != nullptr;
        minimal::client::PushThemeToPlugins(env);
        if (surface_was_live && env.chat_surface == nullptr) {
            app->DisableChatSurface("threw on theme switch");
        }
        minimal::client::SaveThemePreference(env.config_dir, name);
        if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
            app->TabNotice(tab, "theme switched to '" + name + "'");
        }
    };
    // The picker's row set: the stems of <config_dir>/themes/*.json. Sorted so
    // the menu is stable across opens; a missing/unreadable dir yields an empty
    // list (the picker's empty_message covers it -- same degrade rule as every
    // other decoration read).
    auto list_theme_names = [&env]() -> std::vector<std::string> {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(env.config_dir / "themes",
                                                                     ec)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry.path().extension() != ".json") {
                continue;
            }
            names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    // /rewind, /redo and /new are intercepted here rather than routed through
    // SendCommand: /rewind and /redo are client-native ClientEvents (a oneof
    // sibling of cancel/attach), not server-side Commands, so there is no
    // CommandInvocation for the server to run -- and the client also has to
    // resolve "N user turns back" to a seq itself, the server never told it
    // one. /new is a client tab operation (open + focus a fresh sessionless
    // tab, distinct from a subagent spawn) the server has no part in either.
    //
    // SESSIONLESS TABS (no session yet): the commands the server catalog
    // marks requires_session are answered client-side -- a session is never
    // created for a command that cannot use one. The genuinely sessionless
    // commands go over RunSessionlessCommand, and /sessions' bare form is
    // answered outright client-side (its server-side result was only ever a
    // view_hint pointing at the same picker -- the round trip minted the
    // junk session and bought nothing). /agent //model never reach this
    // guard: they are the desired-state commands above, usable on any tab.
    auto submit_command = [app, &command_catalog, commands_listed, focused_or_new_tab,
                           &show_commands_help, &open_keymap_help_fn, &close_tab,
                           &open_picker_fn, &open_agent_picker_fn,
                           &open_model_picker_fn, &open_subagent_model_picker_fn,
                           &open_mcp_picker_fn, &open_theme_picker_fn,
                           &apply_theme](const std::string& line) {
        // Bare /help is answered CLIENT-SIDE from the ListCommands catalog:
        // the server's listing reached the transient notice line, whose
        // viewport caps at a handful of rows -- with more commands than that,
        // the tail was simply unreachable (no scrolling, no surface). The
        // overlay is filterable and scrollable, and reads the same live
        // catalog the /-autocomplete offers. "/help <command>" still goes to
        // the server: a single command's usage/summary is one short notice.
        if (line == "/help") {
            // Opens an overlay rather than setting a notice, so nothing here
            // would otherwise clear a stale one (an old /cost table, a
            // previous error) left sitting above the editor once the overlay
            // closes.
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->ClearTabNotice(tab);
            }
            show_commands_help = true;
            return;
        }
        // Bare /keymap opens the same overlay the help.overlay binding
        // (F1) opens: the full action -> key listing lives client-side
        // (defaults + the user's keybindings.json + plugin folds), so the
        // server has nothing to answer with. Any arguments are accepted and
        // ignored -- there is no per-argument detail to show -- rather than
        // falling through to RunSessionlessCommand/SubmitCommand below,
        // which would round-trip "/keymap <arg>" to a server that never
        // registered an argument form for it (command_catalog's "keymap"
        // entry only exists to make bare /keymap autocomplete-discoverable).
        if (line == "/keymap" || line.rfind("/keymap ", 0) == 0) {
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->ClearTabNotice(tab);
            }
            open_keymap_help_fn();
            return;
        }
        const std::string sid = app->FocusedSessionId();
        const bool has_session = !sid.empty();
        if (line == "/pause" || line == "/resume") {
            // Client-native, like shift+tab's run-mode cycle: both send the
            // SetPausedEvent wire message directly (a /-command would queue
            // for the worker and only land at a turn boundary -- precisely
            // the delay pause exists to avoid). The current state comes from
            // the tab's metadata snapshot; the confirming metadata event
            // refreshes the status bar's paused segment. Explicit /resume
            // with nothing queued kicks the seedless continuation server-side
            // (Session::SetPaused).
            if (!has_session) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->TabNotice(tab, line.substr(1) + " needs a session; send a message to "
                                            "start one first");
                }
                return;
            }
            const bool paused = app->GetDetails(sid).paused;
            const bool want_paused = line == "/pause";
            if (paused == want_paused) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->TabNotice(tab, want_paused ? "already paused" : "not paused");
                }
                return;
            }
            app->SendSetPaused(sid, want_paused);
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->TabNotice(tab, want_paused ? "pausing -- the agent stops after its current "
                                                  "tool call"
                                                : "resumed");
            }
            return;
        }
        if (line == "/redo" || line == "/rewind" || line.rfind("/rewind ", 0) == 0) {
            // Refused VISIBLY on a sessionless tab: SendRewind with an empty
            // sid hits GetState("") == null and silently returns -- the user
            // pressed a key and nothing at all happened.
            if (!has_session) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->TabNotice(tab, "/" + line.substr(1, 6) +
                                            " needs a session; send a message to start one first");
                }
                return;
            }
            if (line == "/redo") {
                app->SendRewind(sid, std::nullopt, /*redo=*/true);
                return;
            }
            int n = 1;   // default: undo the most recent user turn
            bool valid_n = true;
            if (line.size() > 8) {
                const std::string arg = line.substr(8);
                try {
                    std::size_t consumed = 0;
                    n = std::stoi(arg, &consumed);
                    // Reject a partially-numeric arg ("2x" -- stoi stops at
                    // the first non-digit and silently returns 2) and a
                    // non-positive one, rather than acting on a depth the
                    // user never actually typed.
                    if (consumed != arg.size() || n <= 0) valid_n = false;
                } catch (const std::exception&) {
                    valid_n = false;   // "/rewind all", overflow, no digits at all
                }
            }
            if (!valid_n) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->TabNotice(tab, "/rewind needs a positive number of turns, e.g. "
                                        "/rewind 2");
                }
                return;
            }
            // nullopt covers both "fewer than n user turns exist in the
            // loaded window" and FindRewindTargetSeq's own refusal when the
            // window is trimmed (scrolled far up -- its front is no longer
            // the true newest message, so it cannot answer reliably).
            if (const auto seq = app->FindRewindTargetSeq(sid, n)) {
                app->SendRewind(sid, *seq, /*redo=*/false);
            } else if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->TabNotice(tab, "/rewind: could not find that many turns in the loaded "
                                    "history");
            }
            return;
        }
        if (line == "/new") {
            // Always spawns a NEW SESSIONLESS tab, focused tab or not --
            // that's the point (a sibling workspace, not a reuse of
            // whatever's already open). Nothing is created server-side until
            // the first message names it.
            app->SetFocusedTab(app->OpenTab(DefaultCreateOptions()));
            return;
        }
        if (line == "/detach-session") {
            // Client-native, like /new: detaching is a TAB operation (end
            // the group's streams with an explicit EndSession{DETACH}, erase
            // the tabs), not a server Command -- the server has no part in
            // the client's tab model. The group is TabsToClose's resolution:
            // the focused root plus its subagent children, or -- invoked
            // from a child tab -- the whole group, parent included. The
            // sessions keep running server-side (work in flight finishes
            // unattended) and /sessions reattaches them while they live.
            // No confirmation: nothing is destroyed.
            const std::string focused = app->FocusedTabId();
            if (focused.empty()) return;
            const auto ids = minimal::client::TabsToClose(app->GetAllTabs(), focused);
            if (!has_session) {
                // A sessionless tab has nothing to detach; closing it is the
                // whole job (same as ctrl+w on an idle tab, minus staging).
                close_tab(focused);
                return;
            }
            app->EndTabSessions(ids, minimal::client::EndSessionMode::kDetach);
            for (const auto& id : ids) close_tab(id);
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->TabNotice(tab, "detached -- /sessions reattaches a live session");
            }
            return;
        }
        // THE DESIRED-STATE COMMANDS. /agent and /model work identically on
        // every tab -- sessionless or not -- which is what makes choosing
        // what a session starts on discoverable at all:
        //   * bare: opens the picker (client-owned UI, zero round trip; the
        //     server's bare forms still exist for headless clients).
        //   * named: sets the tab's DESIRED state (AppState::Stage*). On a
        //     sessionless tab it rides CreateSessionRequest; on a sessioned
        //     one it converges at the next turn start. It is never a direct
        //     switch, so the status bar shows the fresh value immediately
        //     (pending until the metadata event confirms).
        const auto parsed = minimal::client::SplitCommand(line);
        // THEME (see apply_theme above): bare opens the picker, named swaps
        // directly. Deliberately BEFORE the sessionless guard below -- a theme
        // switch is decoration, not session state, and works on any tab.
        if (parsed.name == "theme") {
            if (parsed.positional.empty()) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->ClearTabNotice(tab);
                }
                open_theme_picker_fn();
            } else {
                apply_theme(parsed.positional.front());
            }
            return;
        }
        if (parsed.name == "agent" || parsed.name == "model") {
            if (parsed.positional.empty()) {
                if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                    app->ClearTabNotice(tab);
                }
                if (parsed.name == "agent") {
                    open_agent_picker_fn();
                } else {
                    open_model_picker_fn();
                }
                return;
            }
            const std::string& value = parsed.positional.front();
            if (const std::string tab = focused_or_new_tab(); !tab.empty()) {
                if (parsed.name == "model") {
                    // Shape-check the ref here (SplitModelRef, the same
                    // parser the headless --model flag uses) so a typo
                    // stages nothing; whether it RESOLVES is still the
                    // server's call, at create or at turn start.
                    if (!minimal::common::SplitModelRef(value)) {
                        app->TabNotice(tab, "'" + value +
                                                "' must be <provider>/<model>, e.g. "
                                                "local/qwen4 (or run /model with no "
                                                "arguments)");
                        return;
                    }
                    app->StageModel(tab, value);
                } else {
                    app->StageAgentPreset(tab, value);
                }
                app->TabNotice(tab, std::string(parsed.name == "agent" ? "agent '" : "model '") +
                                        value + "' will be used on your next message");
            }
            return;
        }
        // /subagent-model is NOT a desired-state command: its subject is a
        // session's own setting (what model its spawned children run), so it
        // has nothing to stage on a sessionless tab. The bare form opens the
        // picker directly on a sessioned tab (the same zero-round-trip open
        // as bare /model); everything else -- the named form on either kind
        // of tab, and the bare form without a session -- falls through to the
        // server command stream, which validates the ref and answers the
        // sessionless-tab refusal.
        if (parsed.name == "subagent-model" && parsed.positional.empty() && has_session) {
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->ClearTabNotice(tab);
            }
            open_subagent_model_picker_fn();
            return;
        }
        if (!has_session) {
            if (line == "/sessions") { open_picker_fn(); return; }
            // /mcp is process-global like /sessions -- no per-tab desired
            // state to stage, so the bare form is answered the same way:
            // client-side, no round trip, no junk sessionless-tab session.
            if (line == "/mcp") { open_mcp_picker_fn(); return; }
            // Everything else the catalog marks requires_session is refused
            // here, visibly, without creating a session. A name ABSENT from
            // the catalog is reported as what it is -- unknown -- rather
            // than riding the session notice (a removed command reading as
            // "start a session first" was exactly the /providers
            // confusion). Only when the catalog actually arrived, though:
            // after a failed ListCommands it is empty and knows nothing.
            bool known = false;
            bool sessionless = false;
            for (const auto& entry : command_catalog) {
                if (entry.name == parsed.name) {
                    known = true;
                    sessionless = !entry.requires_session;
                    break;
                }
            }
            if (const std::string tab = focused_or_new_tab(); !tab.empty()) {
                if (!known && commands_listed) {
                    app->TabNotice(tab, "unknown command: /" + parsed.name + " (try /help)");
                } else if (!sessionless) {
                    app->TabNotice(tab, "/" + parsed.name +
                                            " needs a session; send a message to start one first");
                } else {
                    app->RunSessionlessCommand(tab, line);
                }
            }
            return;
        }
        app->SubmitCommand(app->FocusedTabId(), line);
    };
    // Bracketed-paste flag, shared with HandleGlobalKey (keybinding_dispatch.cpp)
    // via KeyContext::pasting: a macOS Option-glyph byte pasted verbatim
    // (key_event_parser.cpp's IsOptionGlyphEvent) is byte-identical to the
    // key it would otherwise bind, so HandleGlobalKey needs to know a paste
    // is in progress to defer to the editor instead of firing the action.
    const auto pasting = std::make_shared<bool>(false);
    auto editor = minimal::client::MakePromptEditor(
        theme, prompt_state,
        submit_chat,
        submit_command,
        suggest,
        // Explicit trailing return type: a bare `{ return command_catalog; }`
        // deduces BY VALUE (auto return-type deduction never deduces a
        // reference), silently copying the whole catalog on every call --
        // exactly the per-keystroke cost CommandCatalogSource's contract
        // exists to avoid. command_catalog is a RunTui local that outlives
        // `editor`'s use, same lifetime argument as `state`/`theme` above.
        [&command_catalog]() -> const std::vector<minimal::client::CommandCatalogEntry>& {
            return command_catalog;
        },
        // The requires_session filter's probe (cheap: one locked read in
        // AppState). An empty FocusedSessionId means no tab, or a tab whose
        // session does not exist yet -- both hide the session-bound entries.
        // The subagent probe feeds the read-only gate (UsableOnSubagent):
        // a child's command surface is read-only server-side, so a subagent
        // tab is offered only what would not be refused there.
        [&app]() { return !app->FocusedSessionId().empty(); },
        [&app]() { return app->FocusedTabIsSubagent(); },
        pasting);

    // --- permission modal (inner swapped per outstanding request) -------------
    // The host is a STABLE component in the tree; its inner component is
    // rebuilt when a new request_id arrives. Checkbox state lives in
    // GrantToggles (keyed by request in tab_ui), so it survives the rebuild.
    // MakeModalHost binds show_permission by reference -- a by-value capture
    // latched `false` forever here once: the prompt rendered but no key ever
    // reached it (the "frozen UI" class). Same for the confirmation and
    // picker hosts below.
    auto permission_child = std::make_shared<Component>();
    auto permission_host_interactive =
        minimal::client::MakeModalHost(permission_child, show_permission);

    // The server confirmation modal (/clear asks through the
    // ConfirmationBroker, which BLOCKS the session worker until answered):
    // same host pattern as the permission prompt -- a STABLE component whose
    // inner modal is rebuilt when a new request_id arrives. The answer path
    // (AnswerConfirmation) existed and was dead code; nothing rendered the
    // request, so the tab wedged in Waiting until some unrelated event
    // arrived. Security-adjacent: it nests directly below the permission
    // prompt in the stack (see the modal stack below -- the two are mutually
    // exclusive in SessionState, so they never compete for the layer).
    auto confirmation_child = std::make_shared<Component>();
    auto confirmation_host_interactive =
        minimal::client::MakeModalHost(confirmation_child, show_confirmation);

    // The model's ask modal (tool-ask plugin): same stable-host pattern. A
    // question can only arrive while its session's worker is parked inside
    // the ask tool, so like the confirmation modal it nests below the
    // security prompts -- the three pending modal kinds are mutually
    // exclusive in SessionState.
    bool show_ask = false;
    auto ask_child = std::make_shared<Component>();
    auto ask_host_interactive = minimal::client::MakeModalHost(ask_child, show_ask);

    // --- quit / close confirmation --------------------------------------------
    // THREE answers, not two (the close-vs-detach split): "close anyway"
    // destroys the sessions and everything in flight (EndSession{TERMINATE});
    // "detach" ends only the streams -- the sessions keep running
    // server-side, work finishes unattended, the server shuts itself down
    // once nothing is busy; "cancel" (esc) backs out.
    PendingConfirmation confirm_request;
    confirm_request.kind = "app_quit";
    confirm_request.title = "work is still running";
    confirm_request.detail =
        "a session still has work in flight. close: destroy it now. "
        "detach: keep it running in the background (/sessions reattaches).";
    confirm_request.options = {"close anyway", "cancel", "detach"};
    auto confirm_modal = minimal::client::MakeConfirmationModal(
        confirm_request, theme, [&](const std::string& chosen) {
            if (chosen != "close anyway" && chosen != "detach") {
                show_quit_confirm = false;
                pending_close = PendingClose{};
                return;
            }
            // State the intent on the wire FIRST, while the streams are
            // still open: DETACH or TERMINATE per the answer. Then close the
            // tabs root first, children after (the staging order -- see
            // PendingClose); each Close() lands on an already-ended stream
            // and degrades to a no-op.
            app->EndTabSessions(pending_close.session_ids,
                                chosen == "detach" ? minimal::client::EndSessionMode::kDetach
                                                   : minimal::client::EndSessionMode::kTerminate);
            for (const auto& id : pending_close.session_ids) close_tab(id);
            if (pending_close.quit) quit();
            show_quit_confirm = false;
            pending_close = PendingClose{};
        });

    // --- session picker (inner rebuilt on each open) ---------------------------
    auto picker_child = std::make_shared<Component>();
    auto picker_host_interactive = minimal::client::MakeModalHost(picker_child, show_picker);

    // --- agent preset picker ---------------------------------------------------
    auto agent_picker_child = std::make_shared<Component>();
    auto agent_picker_host_interactive =
        minimal::client::MakeModalHost(agent_picker_child, show_agent_picker);

    // --- model picker -----------------------------------------------------------
    auto model_picker_child = std::make_shared<Component>();
    auto model_picker_host_interactive =
        minimal::client::MakeModalHost(model_picker_child, show_model_picker);

    // --- mcp server picker -------------------------------------------------------
    auto mcp_picker_child = std::make_shared<Component>();
    auto mcp_picker_host_interactive =
        minimal::client::MakeModalHost(mcp_picker_child, show_mcp_picker);

    // --- theme picker ------------------------------------------------------------
    // Like the pickers above: a stable host whose inner component is rebuilt
    // on each open (open_theme_picker, below) -- themes are read from disk per
    // open, so a theme dropped into the themes dir shows up without a restart.
    auto theme_picker_child = std::make_shared<Component>();
    auto theme_picker_host_interactive =
        minimal::client::MakeModalHost(theme_picker_child, show_theme_picker);

    // T8: typing on an empty app already starts a session directly
    // (submit_chat, above); the picker's "+ new session" row is the same
    // action reached from inside the modal, with no text to send yet. The
    // tab it opens is SESSIONLESS -- nothing is minted server-side until the
    // first message (submit_chat -> SubmitChatTurn -> EnsureSession), so an
    // abandoned "+ new" tab leaves no row behind.
    auto open_new_session = [&] {
        show_picker = false;
        if (const auto tab = app->OpenTab(DefaultCreateOptions()); !tab.empty()) {
            app->SetFocusedTab(tab);
        }
    };

    // Owns picker_data/agent_picker_presets and the fetch/rebuild/open
    // flows privately; writes rebuilt components into picker_child/
    // agent_picker_child (declared above -- the modal slots RunTui's own
    // frame assembly and MakeModalHost calls read from, unchanged).
    minimal::client::PickerController picker_controller(
        minimal::client::PickerController::Dependencies{
            *client, *app, theme, picker_child, agent_picker_child, model_picker_child,
            mcp_picker_child, show_picker, show_agent_picker, show_model_picker,
            show_mcp_picker, staged_delete_id,
            show_delete_confirm, open_new_session,
            // The desired-state pickers stage onto this -- the focused tab,
            // or (the empty startup app, zero tabs) a fresh sessionless one,
            // so a /agent //model choice made before any tab exists lands on
            // the tab the first message will use.
            focused_or_new_tab});
    auto open_picker = [&] { picker_controller.OpenSessionPicker(); };
    auto open_agent_picker = [&] { picker_controller.OpenAgentPicker(); };
    auto open_model_picker = [&] { picker_controller.OpenModelPicker(/*refresh=*/false); };
    auto open_subagent_model_picker = [&] { picker_controller.OpenSubagentModelPicker(); };
    auto open_mcp_picker = [&] { picker_controller.OpenMcpPicker(); };
    // Bind the submit_command intercepts (declared before, assigned now --
    // they run only on user input, long after this line).
    open_picker_fn = open_picker;
    open_agent_picker_fn = open_agent_picker;
    open_model_picker_fn = open_model_picker;
    open_subagent_model_picker_fn = open_subagent_model_picker;
    open_mcp_picker_fn = open_mcp_picker;
    auto open_theme_picker = [&] {
        *theme_picker_child = minimal::client::MakeThemePicker(
            list_theme_names(), theme.name(), theme,
            minimal::client::ThemePickerCallbacks{
                [&](const std::string& name) {
                    show_theme_picker = false;
                    apply_theme(name);
                },
                [&] { show_theme_picker = false; }});
        show_theme_picker = true;
    };
    open_theme_picker_fn = open_theme_picker;
    // Reset slots for the two help overlays: they are built once, so without
    // these the typed filter survived across opens -- reopening the help
    // presented the previous needle as if it were a fresh search.
    std::function<void()> keymap_help_reset;
    std::function<void()> commands_help_reset;
    auto open_keymap_help = [&] {
        if (keymap_help_reset) keymap_help_reset();
        show_keymap_help = true;
    };
    open_keymap_help_fn = open_keymap_help;

    // Built once, unlike the pickers above: env.keybindings is already final
    // by the time RunTui runs (BuildTuiEnvironment folds in the user's file
    // and every plugin view's EnsureDefault before returning), so this needs
    // none of PickerController's async-rebind machinery -- same reasoning as
    // confirm_modal/delete_modal below.
    auto keymap_help_view = minimal::client::MakeKeymapHelpView(
        env.keybindings.all(), theme, [&] { show_keymap_help = false; },
        &keymap_help_reset);

    // Built once, like keymap_help_view: the catalog was fetched once at
    // connect and never changes (see its comment), and `theme` is a RunTui
    // local that outlives the frame. Opened by the bare `/help` interception
    // in submit_command -- the notice line's handful of rows could not hold
    // the full command listing.
    auto commands_help_view = minimal::client::MakeCommandsHelpView(
        command_catalog, theme, [&] { show_commands_help = false; },
        // Same requires_session exclusion the autocomplete applies: while the
        // focused tab has no session, the overlay lists only sessionless
        // commands (the overlay rebuilds its rows on every open, so the probe
        // is fresh each time). The subagent probe likewise: on a child tab,
        // only read-only commands are listed.
        [&app]() { return !app->FocusedSessionId().empty(); },
        [&app]() { return app->FocusedTabIsSubagent(); },
        &commands_help_reset,
        // Enter inserts "/name " with the caret after it, ready for args.
        [&prompt_state](const std::string& name) {
            prompt_state.content = "/" + name + " ";
            prompt_state.cursor_position = static_cast<int>(prompt_state.content.size());
        });

    // --- plugin views (panel region + modal layer) -----------------------------
    // Panels: rendered in the frame beside the tools panel; one shared
    // CatchEvent (over the frame) routes events to visible ones. Modals: one
    // host in the modal stack BELOW the quit confirmation -- a plugin modal
    // can never preempt the security UI, which is the invariant the nesting
    // itself enforces (see the modal stack below).
    auto plugin_modal_child = std::make_shared<Component>();
    // NOT MakeModalHost: a throwing PLUGIN modal must hide itself for the
    // frame (never fatal), so the render side keeps its own containment.
    auto plugin_modal_host = Renderer([=] {
        if (!*plugin_modal_child) return emptyElement();
        try {
            return (*plugin_modal_child)->Render();
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().error("plugin modal threw: {}", error.what());
        } catch (...) {
            minimal::common::DefaultLogger().error("plugin modal threw a non-standard exception");
        }
        return emptyElement();
    });
    auto plugin_modal_host_interactive = CatchEvent(plugin_modal_host, [&](const Event& event) {
        if (!show_plugin_modal || !*plugin_modal_child) return false;
        // Host-level close, ahead of the plugin: FTXUI's Modal() gives this
        // component exclusive input while show_plugin_modal is true, so
        // HandleGlobalKey's own toggle keybinding -- "main" in Modal()'s
        // terms -- can never run again to close it (see
        // ClosePluginModal's header comment). A plugin whose own component
        // neither consumes Escape nor otherwise clears the flag would
        // otherwise trap every key until the terminal is killed.
        if (event == Event::Escape) {
            minimal::client::ClosePluginModal(plugin_views, plugin_modal_child, show_plugin_modal);
            return true;
        }
        // Plugin component code: a throw escaping into the ftxui loop would
        // std::terminate the process. Contained like the render side above --
        // logged, and the event treated as consumed.
        try {
            return (*plugin_modal_child)->OnEvent(event);
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().warn("plugin modal view threw on event: {}",
                                                  error.what());
        } catch (...) {
            minimal::common::DefaultLogger().warn(
                "plugin modal view threw on event (non-standard exception)");
        }
        return true;
    });

    // THE MAINTENANCE PASS: everything the frame needs but must not do from
    // Render -- a fresh focused-tab snapshot, the per-tab streaming/refresh
    // bookkeeping, and the once-per-notice VIEW side-effects (a View-display
    // notice may stage a modal and run a bounded ListSessions RPC; doing
    // that inside the renderer made the frame stall and the staged modal
    // unclosable). Constructed here, not earlier: it needs open_picker/
    // open_agent_picker (PickerController, above) and plugin_modal_child/
    // show_plugin_modal (above). `tab_maintenance` itself must never be
    // moved/copied for the rest of RunTui's lifetime -- Focused()/
    // FocusedDetails() return references into its own members.
    minimal::client::TabMaintenance tab_maintenance(minimal::client::TabMaintenance::Dependencies{
        *app, engine, view_registry, plugin_views, plugin_modal_child,
        show_plugin_modal, open_picker, open_agent_picker, open_model_picker,
        open_subagent_model_picker, open_mcp_picker,
        [&picker_controller] { picker_controller.Poll(); }, tab_ui, theme,
        message_registry, renderer_registry, display_registry, permission_child, show_permission,
        confirmation_child, show_confirmation, ask_child, show_ask,
        // The chat views this builds name these keys in their own text; the
        // user's keymap decides them (FrameRenderer's fallback builder gets
        // the same two).
        env.keybindings.KeyFor("chat.toggle_collapse", "alt+c"),
        env.keybindings.KeyFor("chat.load_older", "pageup"),
        // Click-to-cancel for QUEUED message rows: resolve the tab's session
        // at click time (a tab's ChatView may predate its session).
        [app](const std::string& tab_id, std::uint64_t turn_id) {
            app->SendCancelQueuedTurnByTab(tab_id, turn_id);
        }});
    // Same names the inline code declared directly -- every lambda below
    // this point (with_keys' KeyContext, the still-inline render body)
    // captures `[&]`, i.e. by ordinary lexical scoping.
    const std::string& focused = tab_maintenance.Focused();
    const SessionState::Snapshot& focused_details = tab_maintenance.FocusedDetails();

    // --- session picker delete confirmation ------------------------------------
    // Built once, like the quit confirmation; `delete_request` is mutated when
    // a delete is staged (single-threaded loop, so the write is safe).
    PendingConfirmation delete_request;
    delete_request.kind = "session_delete";
    delete_request.title = "delete session?";
    delete_request.detail = "the transcript and its subagent sessions are removed permanently";
    delete_request.options = {"delete", "cancel"};
    auto delete_modal = minimal::client::MakeConfirmationModal(
        delete_request, theme, [&](const std::string& chosen) {
            const bool confirmed = chosen == "delete";
            const std::string id = staged_delete_id;
            if (confirmed && !id.empty()) {
                // Off the loop thread: DeleteSession carries a 2 s deadline
                // (session_client.cpp), and this callback fires from inside
                // the event loop -- a slow server used to stall the whole
                // frame for the duration. `client` is captured by shared_ptr
                // (not `&`): the job may still be in AppState's queue when
                // this scope ends, and app->StopRpcQueue() (below, before
                // teardown) joins the worker before `client` itself would go
                // away, so the copy just needs to survive until then. NOT
                // stderr on failure: the alternate screen is live (see
                // ScreenInteractive::FullscreenAlternateScreen() below), and
                // an out-of-band stdout/stderr write from the worker thread
                // would desync the repaint the same way a resize does, and
                // the message was invisible behind the fullscreen buffer
                // regardless -- logged instead, same as every other
                // background-job failure in this file.
                app->RunOnRpcQueue([client, id] {
                    if (const auto status = client->DeleteSession(id); !status.ok()) {
                        minimal::common::DefaultLogger().error("delete failed: {}",
                                                               status.error_message());
                    }
                });
            }
            staged_delete_id.clear();
            show_delete_confirm = false;
            // The confirm is only ever staged FROM the session picker (which
            // it hides for its duration -- it sits below the picker in the
            // modal stack above), so resolving it returns there. Cancel:
            // nothing changed, so the picker's slot component is untouched
            // and its filter/cursor/sort survive as the user left them.
            // Confirmed delete: re-fetch so the removed row drops out of the
            // list (that rebuild starts a fresh filter and cursor -- the
            // same tradeoff the picker's ctrl+a toggle already accepts).
            show_picker = true;
            if (confirmed && !id.empty()) {
                picker_controller.RefreshSessionPicker();
            }
        });

    // --- first run --------------------------------------------------------------
    // T8 used to create a session here unconditionally, whether or not prior
    // sessions existed for the cwd -- avoiding a picker-first modal plus TWO
    // ListSessions round trips (a limit=1 probe here, then the picker's own
    // limit=200 fetch) for the common "start working" case. That traded a
    // startup CreateSession call every launch for a real cost: picking an
    // EXISTING session from ctrl+o left the just-created empty one open as a
    // second, junk tab (and a junk row in the sessions table). Startup creates
    // NOTHING -- and, since tabs became sessionless containers, neither does
    // anything else until the first message: every create path (typing, /new,
    // the picker's "+ new session") now opens a TAB only, and EnsureSession
    // mints the session when the user actually submits (see the sessionless-
    // tabs block above submit_chat). A command run on a sessionless tab never
    // mints one at all: sessionless commands ride RunSessionlessCommand,
    // session-bound ones are refused client-side, and the picker-opening bare
    // forms open their pickers directly.

    // --- frame -----------------------------------------------------------------
    int panel_height = 12;
    // AnyWorkInFlight (keybinding_dispatch.hpp): the caller's tab snapshots
    // carry the state already -- re-snapshotting every tab (a full deep copy)
    // per keypress was pure waste.

    // Quit = close every tab, with ONE confirmation if any work is in flight
    // (ARCHITECTURE.md "Session lifetime"). Shared by ctrl+q, tab-close's
    // sibling, and ctrl+c-when-idle (raw mode means ctrl+c reaches the loop
    // as a key event, not as SIGINT -- without this it was simply dead).
    auto request_quit = [&] {
        const auto tabs = app->GetAllTabs();
        std::vector<std::string> ids;
        for (const auto& tab : tabs) ids.push_back(tab.tab_id);
        // HasQueuedWork: a sessionless tab's EnsureSession job can still be
        // QUEUED (not yet started, so SessionStateKind hasn't moved off Idle
        // -- AnyWorkInFlight can't see it) when quit is pressed. StopRpcQueue
        // DISCARDS the queue rather than draining it, so without this check
        // a message typed moments before quit was silently dropped.
        if (!AnyWorkInFlight(tabs, ids) && !app->HasQueuedWork()) {
            quit();
        } else {
            pending_close = PendingClose{};
            pending_close.quit = true;
            pending_close.session_ids = std::move(ids);
            show_quit_confirm = true;
        }
    };

    // FrameRenderer owns the tab body / status-bar / plugin-panel assembly;
    // constructed once, referencing RunTui's locals the same way
    // TabMaintenance/PickerController do. `frame_renderer` itself must never
    // be moved/copied for the rest of RunTui's lifetime (its Dependencies
    // hold references into RunTui's own objects, resolved once at
    // construction). The permission/confirmation modal rebuild lives in
    // TabMaintenance now (Part 3.2) -- FrameRenderer no longer touches
    // permission_child/show_permission/confirmation_child/show_confirmation.
    minimal::client::FrameRenderer frame_renderer(minimal::client::FrameRenderer::Dependencies{
        *app, theme, focused, focused_details, tab_ui, start_view_registry, message_registry,
        renderer_registry, display_registry, status_registry, engine, show_panel, panel_height,
        env.ui.tools_panel_stdout_buffer_lines > 0 ? env.ui.tools_panel_stdout_buffer_lines : 100,
        editor, status_format, plugin_views,
        // Row-renderer collapse hints follow the user's keymap, not a
        // hardcoded alt+c -- and so do the other key names the frame spells
        // out.
        env.keybindings.KeyFor("chat.toggle_collapse", "alt+c"),
        env.keybindings.KeyFor("session.picker", "ctrl+o"),
        env.keybindings.KeyFor("chat.load_older", "pageup"),
        env.keybindings.KeyFor("chat.scroll_bottom", "pagedown")});
    auto session_view = Renderer(editor, [&] { return frame_renderer.Render(); });

    // --- global keybindings -----------------------------------------------------
    // The dispatch body itself lives in keybinding_dispatch.cpp
    // (HandleGlobalKey) -- constructed ONCE, not per-event: every field is
    // either a reference into a RunTui local that lives for the whole frame
    // loop (so re-deriving it per event would be pure waste, the same
    // argument keymap's own extraction already made) or a std::function
    // capturing those same locals by reference, cheap to build once and
    // reuse. `key_ctx` itself is captured by reference into the CatchEvent
    // lambda below, never copied.
    minimal::client::KeyContext key_ctx{
        *app, keymap, focused,
        show_panel, show_plugin_modal,
        plugin_views, plugin_modal_child,
        open_picker, open_agent_picker, open_model_picker, open_keymap_help, request_quit, close_tab,
        // ChatView::ScrollBy, if the tab has one yet (ui.chat is built
        // lazily inside session_view's own render lambda, so it may not
        // exist on the very first frame of a freshly opened tab).
        [&](const std::string& session_id, int delta_rows) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollBy(delta_rows);
            }
        },
        // The wheel's own path: the gate in ScrollWheelBy bounds how fast a
        // momentum flick can walk the transcript (keyboard scrolling above
        // stays ungated).
        [&](const std::string& session_id, int delta_rows) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollWheelBy(delta_rows);
            }
        },
        [&](const std::string& session_id) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollToBottom();
                it->second.chat->ClearRowFocus();
            }
        },
        // tab.close with work in flight: stage ONE confirmation for the
        // whole closing group, same as request_quit's own busy branch.
        [&](std::vector<std::string> ids) {
            pending_close = PendingClose{};
            pending_close.session_ids = std::move(ids);
            show_quit_confirm = true;
        },
        [&](const std::string& session_id, int delta) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->MoveRowFocus(delta);
            }
        },
        [&](const std::string& session_id) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ToggleCollapseFocused();
            }
        },
        [&](const std::string& session_id) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ToggleCollapseAll();
            }
        },
        // Mouse-click collapse: ToggleCollapseAt consumes the click only
        // when it landed on a collapsible row; a miss falls through.
        [&](const std::string& session_id, int screen_y) -> bool {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                return it->second.chat->ToggleCollapseAt(screen_y);
            }
            return false;
        },
        // Mouse-click cancel of a QUEUED message: CancelPendingAt consumes
        // the click only when a cancellable queued row is under it; a miss
        // falls through to the collapse toggle above's handling order.
        [&](const std::string& session_id, int screen_y) -> bool {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                return it->second.chat->CancelPendingAt(screen_y);
            }
            return false;
        },
        [&](const std::string& session_id) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ToggleTimestamps();
            }
        },
        *pasting,
        [&] { return prompt_state.list_open; }};
    auto with_keys = CatchEvent(session_view, [&](const Event& event) {
        return minimal::client::HandleGlobalKey(key_ctx, event);
    });

    // Modal stack, OUTERMOST = TOPMOST: the security UI (permission prompt)
    // above the wire confirmation above the quit confirmation above the
    // session/agent/model pickers above the delete confirmation above the
    // plugin modals above the frame
    // (with its global keybindings). E4's nesting had the picker outermost;
    // E5 reorders so the never-preempted invariant is actually structural end
    // to end -- no modal layer sits above the permission prompt, and plugin
    // modals sit below everything (E5's own invariant, tested against the
    // fixture plugin).
    //
    // The frame is keyed through with_keys, NOT raw session_view: E5's
    // refactor re-rooted the stack at session_view, silently dropping the
    // CatchEvent wrapper -- every global keybinding (tab cycling, the picker,
    // quit, close, load-older, interrupt) was dead code from E5 until this
    // line was restored to E4's composition.
    Component tree = with_keys;
    tree = Modal(tree, plugin_modal_host_interactive, &show_plugin_modal);
    tree = Modal(tree, delete_modal, &show_delete_confirm);
    tree = Modal(tree, picker_host_interactive, &show_picker);
    tree = Modal(tree, agent_picker_host_interactive, &show_agent_picker);
    tree = Modal(tree, model_picker_host_interactive, &show_model_picker);
    tree = Modal(tree, mcp_picker_host_interactive, &show_mcp_picker);
    tree = Modal(tree, theme_picker_host_interactive, &show_theme_picker);
    tree = Modal(tree, keymap_help_view, &show_keymap_help);
    tree = Modal(tree, commands_help_view, &show_commands_help);
    auto with_quit_confirm = Modal(tree, confirm_modal, &show_quit_confirm);
    // The ask host sits with the confirmation host -- below the security
    // prompts, above the pickers (the security modals must always be able to
    // take the top layer; the pending kinds are mutually exclusive anyway).
    auto with_ask = Modal(with_quit_confirm, ask_host_interactive, &show_ask);
    auto with_confirmation = Modal(with_ask, confirmation_host_interactive,
                                   &show_confirmation);
    auto with_permission = Modal(with_confirmation, permission_host_interactive, &show_permission);

    // The maintenance pass rides the event path ABOVE the whole stack: this
    // handler sees every event the loop processes (including the pump's
    // Event::Custom, even while a modal would consume ordinary keys), runs
    // the maintenance pass, and consumes NOTHING -- a pure pre-render
    // observation point. This is what keeps side-effects out of Render().
    Component final_tree = CatchEvent(with_permission, [&](const Event&) {
        tab_maintenance.Run();
        // A paste can only be IN PROGRESS while the editor is the stack's
        // active leaf; any modal grabbing exclusive routing (FTXUI Modal()'s
        // own contract) swallows the rest of the paste, including its own
        // end marker, before the editor's CatchEvent (prompt_editor.cpp)
        // ever sees it to clear this itself. Self-heal here, the one place
        // that watches every event above the whole stack: the first event
        // after a modal opens mid-paste clears the flag, rather than
        // leaving it stuck true (every later Return silently inserting a
        // newline instead of submitting). tab_maintenance.Run() above
        // already applied this tick's snapshot, so a permission/
        // confirmation/ask modal that just became due is reflected in its
        // show_* flag by the time this check runs.
        if (*pasting && (show_plugin_modal || show_delete_confirm || show_picker ||
                         show_agent_picker || show_model_picker || show_mcp_picker ||
                         show_theme_picker ||
                         show_keymap_help || show_commands_help || show_quit_confirm ||
                         show_ask || show_confirmation || show_permission)) {
            *pasting = false;
        }
        return false;
    });
    // The theme's optional canvas color, outermost so it reaches every modal
    // too. Read per frame: /theme swaps `theme` in place. A theme without
    // `background` leaves the terminal's own (possibly transparent) bg.
    final_tree = Renderer(final_tree, [inner = final_tree, &theme] {
        return inner->Render() | minimal::client::FillBackground(theme.ColorFor("background"));
    });
    tab_maintenance.Run();   // the first frame must not render an empty snapshot

    // Shutdown signal shared by both background threads below: notified once,
    // right after running.store(false), so a thread parked in a cv wait wakes
    // IMMEDIATELY instead of riding out its own poll interval. Only the
    // heartbeat actually waits on it (the pump still must wake every 50ms
    // regardless, to notice a UiVersion() change with no other trigger) --
    // shared rather than duplicated because both threads shut down at the
    // same instant for the same reason.
    std::mutex shutdown_mtx;
    std::condition_variable shutdown_cv;

    // THE PUMP: reader threads wake nothing by themselves; this loop redraws
    // at up to 20 Hz from snapshots. Redraws are gated on the app's UI
    // revision -- an idle TUI used to repaint the full frame 20x per second
    // no matter what. The 50ms wake itself is not optional (nothing else can
    // notice a UiVersion() change or the shutdown flag), but the OLD
    // unconditional "force a repaint every 4th tick regardless" floor is:
    // that existed only so the editor's @-mention debounce gate -- which
    // fires FROM a render pass (prompt_editor.cpp has no timer of its own) --
    // kept getting serviced while the user typed, and paid a full render
    // pass 5x/s for the whole app's life to cover a window that is usually
    // closed. Gated instead on query_pending_hint: a debounced query is
    // actually armed only for the ~100-150ms between a keystroke and its
    // answer, so this floor is now active for a sliver of the app's
    // lifetime rather than always. StreamingMarkdownView::PendingReparseHint
    // is the same seam for the streaming markdown debounce: DispatchToTab
    // bumps UiVersion() once per wire event (state.cpp), so a burst of
    // deltas that PAUSES wakes this pump once for the pause's last delta --
    // if that render landed before the debounce window closed, nothing
    // bumps the version again when it DOES close, and the tail sat
    // rendered-stale until an unrelated event happened to arrive.
    std::atomic<bool> running{true};
    std::thread pump([&] {
        uint64_t last_version = app->UiVersion();
        // Running-tool forced redraws are bucketed to whatever FormatDuration
        // can actually display (tool_status.hpp) instead of firing every
        // 50ms tick: a repaint pass is a full FTXUI tree walk + full-screen
        // terminal write (no dirty-tracking in this FTXUI version), so
        // forcing it 20x/s for the whole life of a long tool call was paying
        // real CPU (~1% idle -> ~4-5% while running) for ticks the display
        // couldn't even show. Sentinel far in the past so the very first
        // running tick fires immediately.
        auto last_running_redraw = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint64_t version = app->UiVersion();
            // FocusedTabFastestRunningToolElapsedMs joins the same "force a
            // repaint with no version bump" seam: a foreground tool that goes
            // quiet (a long `sleep`, a silent build step) produces no wire
            // events at all, so nothing bumps UiVersion -- without this, both
            // the spinner and a live elapsed counter would freeze the moment
            // the tool stopped printing, reading as hung rather than merely
            // slow. Bucketed by elapsed time to match FormatDuration's own
            // resolution: <1s shows whole ms (needs the full 50ms cadence),
            // 1s-60s shows tenths of a second (100ms is enough), >=60s shows
            // whole seconds (1000ms is enough) -- so the redraw rate never
            // outruns what the number on screen can change to.
            bool running_tool_redraw_due = false;
            if (const auto elapsed = app->FocusedTabFastestRunningToolElapsedMs()) {
                const auto interval = *elapsed < std::chrono::milliseconds(1000)
                                           ? std::chrono::milliseconds(50)
                                       : *elapsed < std::chrono::milliseconds(60000)
                                           ? std::chrono::milliseconds(100)
                                           : std::chrono::milliseconds(1000);
                const auto now_steady = std::chrono::steady_clock::now();
                if (now_steady - last_running_redraw >= interval) {
                    running_tool_redraw_due = true;
                    last_running_redraw = now_steady;
                }
            }
            const bool debounce_pending =
                prompt_state.query_pending_hint.load(std::memory_order_acquire) ||
                minimal::client::StreamingMarkdownView::PendingReparseHint().load(
                    std::memory_order_acquire) ||
                running_tool_redraw_due;
            if (version != last_version || debounce_pending) {
                last_version = version;
                screen.PostEvent(Event::Custom);
            }
        }
    });

    // THE HEARTBEAT: the server's idle exit (server.idle_shutdown_seconds,
    // default 5s) fires once zero sessions are live AND zero streams are
    // attached AND no recent Ping arrived. A TUI on its empty-state welcome
    // view -- before its first session, which is created lazily on the first
    // submission -- holds neither, so without a beat the server shut itself
    // down while the user was still composing that first message: the
    // reported "start client, fiddle with /-autocomplete, submit, server
    // stops". One Ping per ~2s marks the client alive; quitting the TUI
    // stops the beats and the server exits on its own linger as designed.
    //
    // A condition_variable wait, not a 100ms-poll/20-tick counter: the old
    // shape woke this thread 20x for every Ping it actually sent, and the
    // shutdown flag was only ever checked between those 100ms slices, so
    // quitting could wait up to 100ms on a thread doing nothing but counting.
    // wait_for's predicate returns true (ending the wait early) the instant
    // shutdown_cv is notified.
    std::thread heartbeat([&] {
        std::unique_lock<std::mutex> lock(shutdown_mtx);
        while (!shutdown_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return !running.load(std::memory_order_acquire);
        })) {
            (void)client->Ping();   // failures ignored -- see SessionClient::Ping
        }
    });

    // Bracketed paste (DECSET 2004): enabled for the duration of the loop, the
    // terminal wraps a paste in ESC[200~ ... ESC[201~ markers. ftxui 7 has no
    // paste support, but it forwards the markers verbatim as Event::Special
    // (unknown CSI -> Special, full bytes preserved), which the prompt editor
    // consumes to treat a multi-line paste as ONE turn instead of one
    // submission per line. RAII, not a post-Loop write: an exception escaping
    // the loop must not leave the terminal in paste mode.
    struct BracketedPasteGuard {
        ~BracketedPasteGuard() {
            std::fputs("\x1b[?2004l", stdout);
            std::fflush(stdout);
        }
    } bracketed_paste_guard;
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);

    // Everything screen.Loop() needs undone before app/env/client unwind, in
    // the order their own comments require -- called once on the normal
    // return path below, and again from the catch block if a key handler (or
    // anything else running under the loop) throws. Without this, an
    // escaping exception skips straight to ~app/~env/~client: pump/heartbeat
    // are still joinable std::thread locals at that point, and a joinable
    // thread's destructor calls std::terminate() immediately -- before any of
    // the ordering below (which exists to avoid a deadlock or a
    // dereference-after-free) ever runs.
    auto shutdown_tui = [&] {
        {
            // Under shutdown_mtx, not a bare store: the heartbeat's
            // wait_for(lock, 2s, predicate) checks the predicate WHILE holding
            // this same lock. Without it, a store+notify landing exactly as the
            // heartbeat finishes checking (still true) but before it actually
            // blocks on the cv can be missed entirely -- bounded by the 2s
            // timeout floor, so not a hang, but a real extra 2s of quit delay
            // for no reason. Taking the lock here guarantees one of two
            // outcomes: the heartbeat is already blocked (wakes immediately), or
            // it hasn't checked yet (acquires the lock after this and sees
            // false right away).
            std::lock_guard<std::mutex> lk(shutdown_mtx);
            running.store(false, std::memory_order_release);
        }
        shutdown_cv.notify_all();   // wakes the heartbeat immediately, not on its next 2s tick
        pump.join();
        heartbeat.join();

        // Join the async RPC worker BEFORE tearing down tabs: a job's completion
        // (RequestScrollback et al.) publishes into SessionState/AppState the
        // same way a reader thread's dispatch does, and DetachAllTabs() below
        // destroys the very tabs a straggling job would publish into.
        //
        // The terminal is already restored by this point (screen.Loop() above
        // has returned or thrown -- either way it is off the stack), and
        // StopRpcQueue()'s join is unbounded against whatever job is currently
        // running (a blocking RPC with its own multi-second deadline --
        // EnsureSession up to 5s, a model-list refresh up to 15s). Without this
        // notice the process just sits there silently for up to that long,
        // looking hung rather than merely finishing up.
        if (app->RpcQueueBusy()) {
            std::fprintf(stderr, "minimal: waiting for a pending request to finish...\n");
        }
        app->StopRpcQueue();

        // S5.4: quitting the TUI DETACHES every remaining tab rather than
        // terminating it -- a session outlives the client that opened it, so
        // closing the terminal must not destroy in-flight work. With the new
        // idle-exit rule the server follows the client: once every detached
        // session is quiescent (and the heartbeat is stale), the linger expires
        // and the process shuts down. The close confirmation's "close anyway"
        // already TERMINATEd its sessions and erased those tabs, so this finds
        // nothing left for them; ctrl+w on an idle group and /detach-session end
        // their streams explicitly before erasing. DetachAllTabs is the catch-all
        // for whatever remains (sessionless tabs contribute nothing).
        app->DetachAllTabs();

        // Detach the focus seam BEFORE `app` unwinds: `app` (owning AppState) is
        // declared after `env` (owning the plugin registries and this provider),
        // so it is destroyed FIRST -- a plugin poller thread still calling
        // Tabs()/FocusedSessionId() during env's own teardown would otherwise
        // dereference a freed AppState. See HostFocusProvider::Detach().
        env.host_focus_provider->Detach();

        // Cancel subscribe streams BEFORE this function's locals unwind: `env`
        // (the plugin environment) is declared after `client`, so it is
        // destroyed FIRST, and a subscribing plugin that joins its worker in its
        // destructor would deadlock on a stream nobody has cancelled -- the
        // client's own destructor sweep only runs after env is already gone.
        client->CancelSubscriptions();
    };

    try {
        screen.Loop(final_tree);
    } catch (const std::exception& error) {
        shutdown_tui();
        minimal::common::DefaultLogger().error("tui: escaping exception: {}", error.what());
        std::fprintf(stderr, "minimal: internal error: %s\n", error.what());
        return kInternalError;
    } catch (...) {
        shutdown_tui();
        minimal::common::DefaultLogger().error("tui: escaping non-standard exception");
        std::fprintf(stderr, "minimal: internal error (non-standard exception)\n");
        return kInternalError;
    }
    shutdown_tui();
    return kOk;
}

}  // namespace
int main(int argc, char** argv) {
    // Matches the server's process-wide disposition: the client writes to
    // gRPC sockets and to stdout -- which a piped script may close early --
    // and neither should kill the process by default disposition. stdout
    // writes after that simply fail; the run completes and the exit status
    // goes where it can.
    std::signal(SIGPIPE, SIG_IGN);

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
        case minimal::client::ParsedArgs::Action::ListSessions:
        case minimal::client::ParsedArgs::Action::Tui:
            break;
    }

    // LOGGING BEFORE ANYTHING ELSE LOGS: DefaultLogger resolves to spdlog's
    // console default on first use, and an out-of-band stdout/stderr write
    // desyncs the alternate screen the same way a resize does (see RunTui);
    // headless stdout carries JSONL. So client logs go to a rotating FILE
    // under the data dir, never the terminal. Configured after the
    // usage/version exits above -- those print to stdout on purpose -- and
    // before any client code runs, per the ordering contract in
    // logging_setup.hpp.
    // A safety net for anything that throws before its own catch can run --
    // DataDir()/ConfigDir() with no $HOME, plugin discovery, BuildTuiEnvironment
    // -- none of which has entered the terminal's raw/alt-screen mode yet, so
    // there is nothing to restore; this only turns a bare SIGABRT (exit 134)
    // into a clean message and exit code. RunTui has its own inner try/catch
    // around screen.Loop() for throws *during* the TUI (see its shutdown_tui).
    try {
        minimal::common::LoggingOptions logging;
        logging.path = (minimal::common::DataDir() / "log" / "client.log").string();
        // File-only: the client owns the terminal, and headless stdout carries
        // JSONL. If the file cannot be opened, ConfigureLogging degrades back to
        // the console sink rather than dropping every message.
        logging.console = false;
        const auto logging_error = minimal::common::ConfigureLogging(logging);
        if (!logging_error.empty()) {
            minimal::common::DefaultLogger().warn("logging: {}", logging_error);
        }

        if (parsed.action == minimal::client::ParsedArgs::Action::Headless) {
            return RunHeadless(parsed.headless);
        }
        if (parsed.action == minimal::client::ParsedArgs::Action::ListSessions) {
            return RunListSessions(parsed.headless);
        }
        return RunTui(parsed);
    } catch (const std::exception& error) {
        minimal::common::DefaultLogger().error("main: escaping exception: {}", error.what());
        std::fprintf(stderr, "minimal: internal error: %s\n", error.what());
        return kInternalError;
    } catch (...) {
        minimal::common::DefaultLogger().error("main: escaping non-standard exception");
        std::fprintf(stderr, "minimal: internal error (non-standard exception)\n");
        return kInternalError;
    }
}
