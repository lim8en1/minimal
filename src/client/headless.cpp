// The non-interactive client: `minimal -p` (one turn, JSONL on stdout) and
// `minimal --list-sessions`.

#include "client_entry.hpp"

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
#include <type_traits>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>


#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>


#include "minimal/client/chat_model.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::client::app {

namespace {

using minimal::client::HeadlessOptions;
using minimal::client::TruncateUtf8;

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
    } else if (event.has_plugin_prompt()) {
        // The headless client answers these UNAVAILABLE (SessionClient); the
        // JSONL line records the prompt so a script's transcript shows why
        // the model reported that no one could answer.
        line["type"] = "plugin_prompt";
        line["request_id"] = event.plugin_prompt().request_id();
        line["kind"] = event.plugin_prompt().kind();
    } else if (event.has_turn_completed()) {
        line["type"] = "turn_completed";
        line["hit_max_turns"] = event.turn_completed().hit_max_turns();
    } else if (event.has_session_ended()) {
        line["type"] = "session_ended";
    } else if (event.has_compaction()) {
        const auto& compaction = event.compaction();
        line["type"] = "compaction";
        line["trigger"] = compaction.trigger();
        line["folded_messages"] = compaction.folded_messages();
        line["tokens_before"] = compaction.tokens_before();
        line["tokens_after"] = compaction.tokens_after();
        line["summary"] = compaction.summary();
        line["skipped"] = compaction.skipped();
        if (!compaction.warning().empty()) line["warning"] = compaction.warning();
    } else {
        line["type"] = "event";
    }
    return line;
}

}  // namespace

int RunHeadless(const HeadlessOptions& options) {
    // Already resolved by ParseArgs -- a bare path became a unix:// target
    // there, so what is printed on failure is what was actually dialled.
    const std::string target =
        options.server.empty() ? minimal::client::DefaultServerTarget() : options.server;

    const auto connected = ConnectOrReport(target, options.autospawn);
    if (!connected) return kUnavailable;
    SessionClient& client = *connected;

    minimal::client::SessionClient::CreateOptions create;
    if (!options.resume_session_id.empty()) {
        // Resume: the stored session keeps its preset and project_root; the
        // server refuses them rather than silently ignoring a conflict.
        create.resume_session_id = options.resume_session_id;
    } else {
        create.project_root = minimal::client::CurrentProjectRootOrEmpty();
        create.agent_preset = options.agent_preset;
        // No name: the server names the session from its first turn
        // (agent::DefaultSessionName), which this prompt is.
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

    const auto connected = ConnectOrReport(target, options.autospawn);
    if (!connected) return kUnavailable;
    SessionClient& client = *connected;

    minimal::client::SessionClient::ListSessionsOptions opts;
    opts.include_all_dirs = options.all_dirs;
    if (!options.all_dirs) opts.project_root = minimal::client::CurrentProjectRootOrEmpty();
    opts.limit = 200;
    auto listed = client.ListAllSessions(opts);
    if (!listed.status.ok()) {
        std::fprintf(stderr, "could not list sessions: %s\n",
                     listed.status.error_message().c_str());
        return kUnavailable;
    }
    const std::vector<minimal::v1::SessionSummary> sessions = std::move(listed.sessions);

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
        const std::time_t seconds = common::UnixMillisToTimeT(unix_ms);
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

}  // namespace minimal::client::app
