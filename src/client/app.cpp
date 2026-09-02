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
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

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
#include "minimal/client/status_format.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/client/render/color_downsample.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"

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
    create.signals = options.signals;

    const auto created = client.CreateSession(create);
    if (!created.status.ok()) {
        std::fprintf(stderr, "%s a session: %s\n",
                     options.resume_session_id.empty() ? "could not create" : "could not resume",
                     created.status.error_message().c_str());
        return kUnavailable;
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
                } else if (event.has_session_ended()) {
                    line["type"] = "session_ended";
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
            // The server-side kill: stop reading and report the run as
            // unavailable. RunTurn's read loop treats this event as terminal
            // exactly like turn_completed, so returning false here just makes
            // the stop explicit from the consumer side too.
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
                            } else if (child_event.has_session_ended()) {
                                line["type"] = "session_ended";
                            } else {
                                line["type"] = "event";
                            }
                            PrintJsonLine(line);
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
        },
        publish_turn_handle);

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
using minimal::client::SessionStateKind;
using minimal::client::Theme;

// What a quit / tab-close confirmation will destroy, when confirmed.
struct PendingClose {
    bool quit = false;
    std::vector<std::string> session_ids;   // ordered children-first
    bool any = false;
};

int RunTui(const minimal::client::ParsedArgs& args) {
    using namespace ftxui;
    namespace plugins = minimal::client::plugins;

    const std::string target = args.headless.server.empty()
                                   ? minimal::client::DefaultServerTarget()
                                   : args.headless.server;
    auto client = std::make_shared<minimal::client::SessionClient>(target);
    if (!client->WaitForConnected(std::chrono::seconds(2))) {
        std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
        std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                     target.rfind("unix://", 0) == 0 ? target.c_str() + 7 : "<socket path>");
        return kUnavailable;
    }

    // The /-command catalog, fetched ONCE at connect (plugins load at startup
    // and config is read once, so the list cannot change mid-connection -- no
    // invalidation machinery). Feeds the prompt editor's /-autocomplete.
    std::vector<minimal::client::CommandCatalogEntry> command_catalog;
    {
        const auto listed = client->ListCommands();
        if (listed.status.ok()) {
            for (const auto& info : listed.response.commands()) {
                command_catalog.push_back(
                    {info.name(), info.summary(), info.read_only(),
                     info.kind() == minimal::v1::COMMAND_KIND_PROMPT_SHORTHAND});
            }
        }
    }

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
    auto& message_registry = env.message_registry;
    auto& status_registry = env.status_registry;
    const auto& status_format = env.status_format;
    const auto& keymap = env.keymap;
    auto& plugin_views = env.plugin_views;

    auto app = std::make_shared<AppState>(client);
    app->SetPluginAttention(&env.attention_state);
    app->SetChatSurface(chat_surface);
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
    auto quit = screen.ExitLoopClosure();

    // --- frame state (loop thread only) --------------------------------------
    PromptEditorState prompt_state;
    bool show_panel = false;
    bool show_picker = false;
    bool show_agent_picker = false;
    bool show_quit_confirm = false;
    bool show_permission = false;
    bool show_confirmation = false;
    bool show_delete_confirm = false;
    bool show_plugin_modal = false;
    std::string staged_delete_id;   // the picker delete confirmation's subject

    // T8: the picker's raw accumulated list, in server delivery order
    // (updated_at DESC, id ASC per session_service.proto). Filter/sort are
    // pure DISPLAY transforms applied fresh by MakeSessionPicker
    // (picker_model.hpp) -- this stays in fetch order so "load more"'s
    // cursor is always derived from the last element the SERVER actually
    // sent, never from whatever the current sort happens to show last.
    struct PickerData {
        std::vector<minimal::v1::SessionSummary> sessions;
        bool include_all_dirs = false;
        bool has_more = false;   // the last fetch returned a full page
    };
    PickerData picker_data;
    // The agent picker's list: fetched whole on each open (no pagination, no
    // sort -- agents/*.md is small and has no "recent" axis).
    std::vector<minimal::v1::AgentPresetSummary> agent_picker_presets;
    PendingClose pending_close;

    // Per-tab UI caches, keyed by session id.
    struct TabUi {
        std::unique_ptr<plugins::MarkdownEngine::Stream> streaming;
        std::size_t streaming_seen = 0;
        std::optional<uint64_t> rendered_version;
        std::unique_ptr<GrantToggles> grants;   // per outstanding request
        std::string grants_for_request;
        // The wire confirmation modal's rebuild key, like grants_for_request:
        // the inner modal is rebuilt only when a NEW request_id arrives.
        std::string confirmation_for_request;
        ftxui::Component surface_body;          // the chat surface's tab body (when slotted)
        plugins::ChatSurface* surface_for = nullptr;   // which surface instance built it
        // Per-tab ChatView: its row cache is keyed by transcript_version, a
        // PER-SESSION counter starting at 0 -- one shared instance served tab
        // A's cached rows as tab B's transcript whenever the two versions
        // collided. One view per tab makes the invariant true by construction.
        std::unique_ptr<minimal::client::ChatView> chat;
        // VIEW-display notice side-effects fire ONCE per notice, keyed on the
        // notice's sequence number (see handle_notice below).
        uint64_t handled_notice_seq = 0;
        // Streaming bookkeeping (see maintain): turn_seq keys recreation,
        // finalized_for_turn fires the engine's one authoritative reparse.
        uint64_t streaming_turn = 0;
        bool finalized_for_turn = false;
        // The newest-slice refresh state machine (T2, reworked): submitted
        // on a version mismatch, resolved via Snapshot::refresh_seq, with
        // the submit/latch/mark-current decision in
        // AdvanceScrollbackRefresh (chat_model.hpp) -- the pure seam where
        // the "user prompt seen only on the first message" bug is now
        // unit-tested.
        minimal::client::ScrollbackRefreshState refresh;
    };
    std::map<std::string, TabUi> tab_ui;

    std::string focused;   // active session id, refreshed every frame
    SessionState::Snapshot focused_details;   // what run_maintenance settled on
    // Gates run_maintenance's own work (below): every AppState mutation --
    // wire events via DispatchToTab, SetFocusedTab, RequestScrollback's async
    // completion, everything -- bumps UiVersion at its own entry point, so
    // "unchanged since the last pass" is a sound proxy for "nothing
    // maintain()/handle_notice() would read has changed either". Sentinel,
    // not 0: forces the first call (before the loop starts) to run regardless
    // of whatever OpenTab's own bumps left UiVersion at.
    uint64_t last_maintenance_ui_version = static_cast<uint64_t>(-1);

    // Per-frame maintenance for the focused tab: feed streaming deltas to the
    // debounced view, and trigger the post-turn scrollback refresh. A turn in
    // flight is NOT fetched around -- the streaming block is the live content
    // until the turn ends and the refresh swaps it for the persisted rows.
    // (Skipped entirely when the chat surface owns the tab body: the surface
    // keeps its own buffers and fetches its own pages.)
    //
    // T2: the refresh itself is now async (AppState::RequestScrollback
    // enqueues it and returns immediately), so this function can no longer
    // re-fetch and observe the result within the same call the way the old
    // synchronous body did -- it returns the snapshot UNCHANGED, and the
    // worker's completion (BumpUiVersion) wakes the pump for a frame that
    // will see the applied page in a freshly-taken snapshot.
    auto maintain = [&](const std::string& session_id,
                        SessionState::Snapshot snap) -> SessionState::Snapshot {
        if (chat_surface != nullptr || app->EffectiveChatSurface() != nullptr) return snap;
        if (snap.session_id.empty() && session_id != snap.session_id) return snap;
        TabUi& ui = tab_ui[session_id];

        const auto make_stream = [&] { return engine.MakeStream(); };

        // Stream recreation keys on turn_seq (SessionState's counter, bumped
        // when a new streaming turn begins), NOT on the old buffer-size
        // heuristic: turn A ending and turn B streaming past A's byte count
        // between two frames spliced B's middle onto A's cached tail,
        // silently.
        if (ui.streaming == nullptr || ui.streaming_turn != snap.turn_seq) {
            ui.streaming = make_stream();
            ui.streaming_seen = 0;
            ui.streaming_turn = snap.turn_seq;
            ui.finalized_for_turn = false;
        } else if (snap.streaming_text.size() > ui.streaming_seen) {
            ui.streaming->AppendDelta(
                std::string_view(snap.streaming_text).substr(ui.streaming_seen));
            ui.streaming_seen = snap.streaming_text.size();
        }

        // The turn's authoritative reparse (the engine contract: Finalize
        // replaces artifacts from mid-stream unclosed spans). Without this
        // wiring the contract was dead code -- the last tokens of a stream
        // rendered only when the debounce's next tick happened to fire.
        if (snap.is_streaming_finalized && !ui.finalized_for_turn && ui.streaming) {
            ui.streaming->Finalize();
            ui.finalized_for_turn = true;
        }

        // Resolve any outstanding refresh and act on the version mismatch --
        // the whole decision (submit / latch / mark-current, success vs
        // failure, streaming vs turn boundary) is AdvanceScrollbackRefresh,
        // unit-tested in chat_model_test.cpp. This call either submits the
        // newest-slice fetch or leaves the bookkeeping current; the fetch
        // itself is async and its application wakes the pump for a later
        // frame.
        if (minimal::client::AdvanceScrollbackRefresh(ui.refresh, snap) ==
            minimal::client::ScrollbackRefreshAction::kSubmit) {
            app->RequestScrollback(session_id, /*newest=*/true);
        }
        return snap;
    };

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
        std::string session_id;
        std::string prefix;
        bool in_flight = false;
        bool ready = false;
        std::vector<std::string> result;
    };
    auto mention_query = std::make_shared<MentionQuery>();
    auto suggest = [app, mention_query](
                       const std::string& prefix) -> std::optional<std::vector<std::string>> {
        const std::string sid = app->FocusedSessionId();
        if (prefix.empty() || sid.empty()) return std::vector<std::string>{};

        std::lock_guard lk(mention_query->mtx);
        if (mention_query->session_id == sid && mention_query->prefix == prefix) {
            if (mention_query->ready) {
                mention_query->in_flight = false;
                mention_query->ready = false;
                return std::move(mention_query->result);
            }
            if (mention_query->in_flight) return std::nullopt;   // still waiting
        }

        // A new (session, prefix) pair: submit it, superseding whatever this
        // slot previously tracked (its completion, if it arrives late, will
        // no longer match session_id/prefix and will be dropped -- see
        // RequestSuggestPaths's callback below).
        mention_query->session_id = sid;
        mention_query->prefix = prefix;
        mention_query->in_flight = true;
        mention_query->ready = false;
        app->RequestSuggestPaths(sid, prefix, 20, [mention_query, sid, prefix](
                                                       std::vector<std::string> paths) {
            std::lock_guard cb_lk(mention_query->mtx);
            if (mention_query->session_id != sid || mention_query->prefix != prefix) {
                return;   // superseded by a newer request; drop the stale answer
            }
            mention_query->result = std::move(paths);
            mention_query->ready = true;
        });
        return std::nullopt;
    };
    // Typing on an empty app STARTS A SESSION (ARCHITECTURE.md "Session
    // picker": with no prior sessions the client creates one directly). The
    // welcome placeholder has always promised this -- but OpenTab was never
    // wired to any UI path, so the promise was dead and a fresh TUI could not
    // hold a conversation at all. OpenTab makes a blocking CreateSession RPC
    // (5s deadline); T2 moves that off the loop thread via AppState's async
    // RPC worker (RunOnRpcQueue) rather than widening OpenTab's own
    // synchronous contract, which the E2E test suite and headless mode both
    // depend on. OpenTab's own locking (and its auto-focus-on-first-tab rule)
    // is safe from any thread, so this is a relocation of WHERE it runs, not
    // a behavior change -- the frame just stays responsive while it runs.
    auto submit_chat = [app](const std::string& text) {
        const std::string sid = app->FocusedSessionId();
        if (sid.empty()) {
            app->RunOnRpcQueue([app, text] {
                minimal::client::SessionClient::CreateOptions opts;
                opts.project_root = std::filesystem::current_path().string();
                if (const auto new_sid = app->OpenTab(opts); !new_sid.empty()) {
                    app->SendChatTurn(new_sid, text);
                }
                // A failed create leaves the app empty; the user sees no new
                // tab and can retry, same visible outcome the synchronous
                // call had on failure.
            });
            return;
        }
        app->SendChatTurn(sid, text);
    };
    // /rewind and /redo are intercepted here rather than routed through
    // SendCommand: the wire treats rewind as a client-native ClientEvent
    // (a oneof sibling of cancel/attach), not a server-side Command, so
    // there is no CommandInvocation for the server to run. Local, since the
    // client also has to resolve "N user turns back" to a seq itself -- the
    // server never told it one.
    auto submit_command = [app](const std::string& line) {
        const std::string sid = app->FocusedSessionId();
        if (line == "/redo") {
            app->SendRewind(sid, std::nullopt, /*redo=*/true);
            return;
        }
        if (line == "/rewind" || line.rfind("/rewind ", 0) == 0) {
            int n = 1;   // default: undo the most recent user turn
            if (line.size() > 8) {
                try {
                    n = std::stoi(line.substr(8));
                } catch (const std::exception&) {
                    n = 1;
                }
            }
            // Silently no-ops when fewer than n user turns exist in the
            // loaded window: FindRewindTargetSeq is the client's only way to
            // resolve a target, and GetState (which OnError needs) is
            // AppState-private -- not worth widening its access for a rare
            // out-of-range /rewind arg.
            if (const auto seq = app->FindRewindTargetSeq(sid, n)) {
                app->SendRewind(sid, *seq, /*redo=*/false);
            }
            return;
        }
        app->SendCommand(sid, line);
    };
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
        });

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

    // The server confirmation modal (/clear and /permission-mode ask through
    // the ConfirmationBroker, which BLOCKS the session worker until answered):
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

    // Tab close, for every caller (keybinding, quit confirmation, quit): the
    // per-tab UI caches must die WITH the tab -- tab_ui entries were never
    // erased, leaking each closed tab's streaming buffer, grants and surface
    // body for the process lifetime.
    auto close_tab = [&](const std::string& id) {
        app->CloseTab(id);
        tab_ui.erase(id);
    };

    // --- quit / close confirmation --------------------------------------------
    PendingConfirmation confirm_request;
    confirm_request.kind = "app_quit";
    confirm_request.title = "destroy running work?";
    confirm_request.detail = "a session still has work in flight; closing destroys it";
    confirm_request.options = {"close anyway", "cancel"};
    auto confirm_modal = minimal::client::MakeConfirmationModal(
        confirm_request, theme, [&](const std::string& chosen) {
            if (chosen != "close anyway") {
                show_quit_confirm = false;
                pending_close = PendingClose{};
                return;
            }
            // Children first, root last (pending_close was staged that way).
            for (const auto& id : pending_close.session_ids) close_tab(id);
            if (pending_close.quit) quit();
            show_quit_confirm = false;
            pending_close = PendingClose{};
        });

    // --- session picker (inner rebuilt on each open) ---------------------------
    auto picker_child = std::make_shared<Component>();
    auto picker_host_interactive = minimal::client::MakeModalHost(picker_child, show_picker);

    // T8: typing on an empty app already starts a session directly
    // (submit_chat, above); the picker's "+ new session" row is the same
    // action reached from inside the modal, with no text to send yet.
    auto open_new_session = [&] {
        show_picker = false;
        app->RunOnRpcQueue([app] {
            minimal::client::SessionClient::CreateOptions opts;
            opts.project_root = std::filesystem::current_path().string();
            if (const auto sid = app->OpenTab(opts); !sid.empty()) app->SetFocusedTab(sid);
        });
    };

    // One page of the picker's list, kept synchronous like DeleteSession
    // below: unlike T2's targets (a per-frame Renderer call, a per-keystroke
    // debounce), this runs once per explicit user action (open, toggle
    // all-dirs, load more) -- the same bounded-pause trade the rest of the
    // one-off calls in this file already make, and mutating picker_data /
    // *picker_child from a worker thread would race the loop thread that
    // owns both.
    //
    // Requests one row beyond the page size to detect a further page: the
    // wire has no dedicated has_more field on ListSessionsResponse.
    static constexpr int32_t kPickerPageSize = 100;
    auto fetch_picker_page = [&](bool reset) {
        if (reset) {
            picker_data.sessions.clear();
            picker_data.has_more = false;
        }
        minimal::client::SessionClient::ListSessionsOptions opts;
        opts.project_root = std::filesystem::current_path().string();
        opts.include_all_dirs = picker_data.include_all_dirs;
        opts.limit = kPickerPageSize + 1;
        if (!reset && !picker_data.sessions.empty()) {
            const auto& last = picker_data.sessions.back();
            opts.before_updated_at = last.updated_at_unix_ms();
            opts.before_id = last.session_id();
        }
        const auto result = client->ListSessions(opts);
        if (!result.status.ok()) return;
        const auto& page = result.response.sessions();
        const int64_t fetched = page.size();
        picker_data.has_more = fetched > kPickerPageSize;
        const int64_t take = picker_data.has_more ? kPickerPageSize : fetched;
        picker_data.sessions.insert(picker_data.sessions.end(), page.begin(),
                                    page.begin() + take);
    };

    // Rebuilds the picker component from picker_data's CURRENT contents --
    // every mutating action (resume/delete/new leave the modal; toggling
    // all-dirs or loading more re-fetches and rebuilds in place, same as
    // the rest of this file's "modal over data it does not own" pattern).
    // std::function, not auto: the body below calls itself (on_toggle_all_dirs
    // / on_load_more), which an auto-deduced lambda type cannot reference
    // inside its own initializer.
    std::function<void()> rebuild_picker_component = [&] {
        *picker_child = minimal::client::MakeSessionPicker(
            picker_data.sessions, picker_data.include_all_dirs, picker_data.has_more, theme,
            {.on_resume =
                 [&](const std::string& session_id) {
                     // A PAST session resumes into its own root group; it is
                     // not a child of whatever is focused.
                     app->AttachTab(session_id);
                     app->SetFocusedTab(session_id);
                     show_picker = false;
                 },
             .on_delete =
                 [&](const std::string& session_id) {
                     // Deletion is destructive and permanent, so it asks --
                     // its own confirmation layer, styled like the quit one.
                     staged_delete_id = session_id;
                     show_delete_confirm = true;
                     show_picker = false;
                 },
             .on_new = open_new_session,
             .on_close = [&] { show_picker = false; },
             .on_toggle_all_dirs =
                 [&] {
                     picker_data.include_all_dirs = !picker_data.include_all_dirs;
                     fetch_picker_page(/*reset=*/true);
                     rebuild_picker_component();
                 },
             .on_load_more =
                 [&] {
                     fetch_picker_page(/*reset=*/false);
                     rebuild_picker_component();
                 }});
    };

    // The picker-open flow, shared by the keybinding and by the /sessions
    // view_hint (a View-displaying CommandResult opens the view named by its
    // hint; the core mapping for "sessions" is this modal). T8: always a
    // fresh first page in the project's own directory -- the "start in the
    // last-used all-dirs state" refinement is not worth the surprise of a
    // picker that sometimes silently shows other projects' sessions.
    auto open_picker = [&] {
        show_picker = true;
        picker_data.include_all_dirs = false;
        fetch_picker_page(/*reset=*/true);
        rebuild_picker_component();
    };

    // --- agent preset picker ---------------------------------------------------
    auto agent_picker_child = std::make_shared<Component>();
    auto agent_picker_host_interactive =
        minimal::client::MakeModalHost(agent_picker_child, show_agent_picker);

    // Rebuilds the agent picker component from agent_picker_presets' current
    // contents -- mirrors rebuild_picker_component, minus everything that
    // doesn't apply to a small, unpaginated, unsorted list.
    auto rebuild_agent_picker_component = [&] {
        *agent_picker_child = minimal::client::MakeAgentPresetPicker(
            agent_picker_presets, theme,
            {.on_select =
                 [&](const std::string& name) {
                     // Re-sends "/agent <name>" over the exact same path a
                     // typed command would take (AppState::SendCommand) --
                     // AgentCommand::Execute's by-name branch, precedence
                     // rules and all, runs unchanged. No new mutation code.
                     app->SendCommand(app->FocusedSessionId(), "agent " + name);
                     show_agent_picker = false;
                 },
             .on_close = [&] { show_agent_picker = false; }});
    };

    // The agent-picker-open flow, shared by the keybinding and by the bare
    // `/agent` view_hint (see handle_notice below) -- one fresh fetch each
    // time, like open_picker: the preset list is small, so there is no
    // pagination state to preserve across opens.
    auto open_agent_picker = [&] {
        show_agent_picker = true;
        agent_picker_presets.clear();
        const auto result = client->ListAgentPresets();
        if (result.status.ok()) {
            const auto& presets = result.response.presets();
            agent_picker_presets.assign(presets.begin(), presets.end());
        }
        rebuild_agent_picker_component();
    };

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
        return (*plugin_modal_child)->OnEvent(event);
    });

    // VIEW-display notices: the side-effect (open the picker, show a plugin
    // modal) fires ONCE per notice here in the maintenance pass, keyed on the
    // notice's sequence number -- never from Render(). The first draft routed
    // this inside the renderer: the notice is never cleared (status text
    // persists until the next result replaces it), so a View notice re-ran
    // open_picker's ListSessions RPC and re-staged the modal at frame rate,
    // making it unclosable. Status/Silent notices keep rendering in the frame
    // below; only their SIDE-EFFECTS are guarded.
    auto handle_notice = [&](const SessionState::Snapshot& snap) {
        if (focused.empty()) return;
        if (!snap.latest_notice.has_value()) return;
        TabUi& ui = tab_ui[focused];
        if (ui.handled_notice_seq == snap.notice_seq) return;   // already handled
        ui.handled_notice_seq = snap.notice_seq;

        const auto& notice = *snap.latest_notice;
        if (notice.display != minimal::v1::COMMAND_DISPLAY_VIEW || notice.view_hint.empty()) {
            return;
        }
        if (notice.view_hint == "sessions") {
            open_picker();   // core mapping: the picker modal
        } else if (notice.view_hint == "agents") {
            open_agent_picker();   // core mapping: bare `/agent`
        } else if (auto* view = view_registry.Find(notice.view_hint)) {
            for (auto& entry : plugin_views) {
                if (entry.view == view) entry.visible = true;
            }
            if (view->spec().placement == plugins::ViewPlacement::Modal) {
                *plugin_modal_child = view->component();
                show_plugin_modal = true;
            }
        }
        // A hint naming no view falls through: the unknown-view-id rule, like
        // the unknown segment id's.
    };

    // THE MAINTENANCE PASS: everything the frame needs but must not do from
    // Render -- a fresh focused-tab snapshot, the maintain() bookkeeping, and
    // the once-per-notice VIEW side-effects (handle_notice may stage modals
    // and run a bounded ListSessions RPC; doing that inside the renderer made
    // the frame stall and the staged modal unclosable). Runs on the loop
    // thread from the event path (see the root CatchEvent below), never from
    // a renderer.
    //
    // Gated on UiVersion (see last_maintenance_ui_version above): this used
    // to run its full body -- a light GetAllTabs() PLUS a full deep
    // GetDetails() protobuf copy of the focused tab's scrollback window (up
    // to scrollback_window_max_messages rows, 2000 by default) -- on EVERY
    // event unconditionally, including every keystroke while typing and
    // every mouse move, none of which touch AppState at all. Skipping when
    // nothing changed leaves `focused`/`focused_details` exactly as the last
    // real pass left them, which is correct BECAUSE nothing changed.
    auto run_maintenance = [&] {
        const uint64_t version = app->UiVersion();
        if (version == last_maintenance_ui_version) return;
        last_maintenance_ui_version = version;

        const auto tabs = app->GetAllTabs();
        focused.clear();
        for (const auto& tab : tabs) {
            if (tab.is_focused) focused = tab.session_id;
        }
        if (focused.empty()) {
            focused_details = SessionState::Snapshot{};
            return;
        }
        // ONE deep snapshot per pass: GetDetails is a full protobuf copy of
        // the scrollback window (up to scrollback_window_max_messages rows),
        // and this used to be paid up to three times per frame. maintain()
        // may replace it with a fresher one if a scrollback refresh fires
        // (it must -- the refresh mutates SessionState out from under the
        // copy this call started with); handle_notice and the renderer both
        // read whatever maintain settled on.
        focused_details = maintain(focused, app->GetDetails(focused));
        handle_notice(focused_details);
    };

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
            if (chosen == "delete" && !staged_delete_id.empty()) {
                // NOT stderr: the alternate screen is live (see
                // ScreenInteractive::FullscreenAlternateScreen() below), and
                // this callback fires from inside the event loop -- any
                // out-of-band stdout/stderr write desyncs the repaint the
                // same way a resize does, and the message was invisible
                // behind the fullscreen buffer regardless.
                if (const auto status = client->DeleteSession(staged_delete_id); !status.ok()) {
                    minimal::common::DefaultLogger().error("delete failed: {}",
                                                           status.error_message());
                }
            }
            staged_delete_id.clear();
            show_delete_confirm = false;
        });

    // --- first run --------------------------------------------------------------
    // T8: startup ALWAYS creates a session directly, whether or not prior
    // sessions exist for the cwd -- the common case is "start working", and
    // the old rule (open the picker whenever any prior session exists)
    // meant that case always paid a modal first, plus TWO ListSessions round
    // trips (a limit=1 probe here, then the picker's own limit=200 fetch).
    // ctrl+o (or the /sessions command) opens the picker on demand, now with
    // its own "+ new session" row for the moment this WAS covering: reaching
    // history from a session already open.
    {
        minimal::client::SessionClient::CreateOptions opts;
        opts.project_root = std::filesystem::current_path().string();
        if (app->OpenTab(opts).empty()) {
            std::fprintf(stderr, "minimal: warning: could not create a session; "
                                 "ctrl+o opens the session picker\n");
        }
    }

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
        for (const auto& tab : tabs) ids.push_back(tab.session_id);
        if (!AnyWorkInFlight(tabs, ids)) {
            quit();
        } else {
            pending_close = PendingClose{};
            pending_close.quit = true;
            pending_close.session_ids = std::move(ids);
            show_quit_confirm = true;
        }
    };

    // Start views: what renders while a session (or the app itself) has no
    // content yet -- the plugin-owned banner slot. All registered views render,
    // stacked in registration order; a THROWING view renders nothing for this
    // frame (the same containment as panel views), never fatal. An empty
    // session_id is the app-level empty state (no tab open).
    auto render_start_views = [&](const std::string& session_id) -> Elements {
        Elements out;
        for (const auto& view : start_view_registry.views()) {
            try {
                out.push_back(view->Render(session_id, theme) | center);
            } catch (const std::exception& error) {
                minimal::common::DefaultLogger().error("start view '{}' threw: {}",
                                                       std::string(view->spec().id),
                                                       error.what());
            } catch (...) {
                minimal::common::DefaultLogger().error(
                    "start view '{}' threw a non-standard exception",
                    std::string(view->spec().id));
            }
        }
        return out;
    };

    auto session_view = Renderer(editor, [&] {
        using namespace ftxui;

        // Tool output (the panel's live tail) only when the panel is
        // actually about to render -- the tab bar below needs name/state
        // only, and this call already happens every render.
        const auto tabs = app->GetAllTabs(/*include_tool_output=*/show_panel);

        // The snapshot (and everything derived from it) was taken by
        // run_maintenance() on this loop iteration's event, BEFORE Render --
        // never here: this lambda must stay side-effect-free and non-blocking.
        // focused_details below is run_maintenance's cached output.

        Elements rows;
        rows.push_back(minimal::client::RenderTabBar(tabs, theme));

        // The frame's DEEP copy: the FOCUSED tab only. (The tools panel reads
        // the light TabSnapshot.tools; nothing renders other tabs' heavy data.)
        if (!focused_details.session_id.empty() || !focused.empty()) {
            const auto& snap = focused_details;
            TabUi& ui = tab_ui[focused];

            // The tab body: the exclusive chat surface when a plugin claimed
            // the slot, the core chat view otherwise. The surface's component
            // is fetched once per tab and cached; the surface's own OnEvent
            // push carries its data (see DispatchToTab). EffectiveChatSurface
            // returns null once a surface has thrown -- the core view takes
            // over and the stale surface_body cache is dropped.
            plugins::ChatSurface* effective_surface = app->EffectiveChatSurface();
            if (effective_surface != nullptr) {
                if (!ui.surface_body || ui.surface_for != effective_surface) {
                    ui.surface_body = effective_surface->component();
                    ui.surface_for = effective_surface;
                }
                try {
                    rows.push_back(ui.surface_body->Render() | yframe | vscroll_indicator | flex);
                } catch (const std::exception& error) {
                    app->DisableChatSurface(error.what());
                    rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                        "takes over") | color(theme.ColorFor("error")));
                } catch (...) {
                    app->DisableChatSurface(nullptr);
                    rows.push_back(text(" chat surface plugin threw -- the core chat view "
                                        "takes over") | color(theme.ColorFor("error")));
                }
            } else {
                // A FRESH session -- nothing persisted, nothing streaming, no
                // earlier pages -- renders the start-view banner in the tab
                // body instead of an empty transcript. has_earlier_messages
                // keeps a resumed session from flashing the banner during the
                // async scrollback window before its first page applies.
                const bool fresh_session = snap.scrollback_page.empty() &&
                                           snap.streaming_text.empty() &&
                                           !snap.has_earlier_messages;
                Elements start = fresh_session ? render_start_views(focused) : Elements{};
                if (!start.empty()) {
                    for (auto& element : start) rows.push_back(std::move(element) | flex);
                } else {
                    if (!ui.chat) {
                        ui.chat = std::make_unique<minimal::client::ChatView>(
                            theme, message_registry, renderer_registry);
                    }
                    // ui.streaming is normally built by maintain()'s stream
                    // recreation (above), but maintain() returns EARLY --
                    // skipping that -- whenever GetDetails raced a tab that
                    // just closed (snap.session_id empty) or a chat-surface
                    // plugin currently owns the tab; a frame landing here
                    // with no start view registered (so `start` stayed
                    // empty) then found ui.streaming still null on its very
                    // first render. Lazily built here too, so *ui.streaming
                    // is never a null deref regardless of which path built it.
                    if (!ui.streaming) ui.streaming = engine.MakeStream();
                    rows.push_back(ui.chat->Render(snap, *ui.streaming) | yframe |
                                   vscroll_indicator | flex);
                }
            }

            // The transient notice line: the focused tab's latest /-command
            // outcome, routed by its CommandDisplay mode (EXTENSIBILITY §4.4).
            // NEVER a chat row -- that is a type-level property of
            // CommandNotice. VIEW side-effects ran in handle_notice() above,
            // once per notice; here a View notice renders nothing (the view
            // it opened is the surface), and STATUS/errors render as the
            // persistent line until the next result replaces them.
            if (snap.latest_notice.has_value()) {
                const auto& notice = *snap.latest_notice;
                if (notice.display != minimal::v1::COMMAND_DISPLAY_VIEW &&
                    notice.display != minimal::v1::COMMAND_DISPLAY_SILENT) {
                    rows.push_back(text(" " + notice.message) |
                                   bgcolor(theme.ColorFor("status_bar_bg")) |
                                   color(notice.is_error ? theme.ColorFor("error")
                                                         : theme.ColorFor("system_message")));
                }
            }

            // The permission modal for the focused tab: rebuilt when the
            // outstanding request changes, shown while it is unanswered.
            if (snap.pending_permission.has_value()) {
                const auto& request = *snap.pending_permission;
                TabUi& ui = tab_ui[focused];
                if (ui.grants_for_request != request.request_id) {
                    ui.grants = std::make_unique<GrantToggles>();
                    ui.grants_for_request = request.request_id;
                    const auto session_id = focused;
                    GrantToggles& toggles = *ui.grants;
                    *permission_child = minimal::client::MakePermissionModal(
                        request, toggles, theme,
                        [app, session_id, request, &toggles](bool allow) {
                            // granted_keys FILTERED to what this request offered
                            // (GrantToggles reads the request's own offers).
                            app->AnswerPermission(session_id, request.request_id,
                                                  allow ? minimal::v1::PERMISSION_DECISION_ALLOW
                                                        : minimal::v1::PERMISSION_DECISION_DENY,
                                                  allow ? toggles.GrantedKeys(request)
                                                        : std::vector<std::string>{});
                        });
                }
                show_permission = true;
            } else {
                show_permission = false;
            }

            // The wire confirmation modal: same rebuild-on-new-request-id rule
            // as the permission modal above. Both are never set at once (each
            // Set clears the other in SessionState).
            if (snap.pending_confirmation.has_value()) {
                const auto& request = *snap.pending_confirmation;
                TabUi& ui = tab_ui[focused];
                if (ui.confirmation_for_request != request.request_id) {
                    ui.confirmation_for_request = request.request_id;
                    const auto session_id = focused;
                    const auto request_copy = request;
                    *confirmation_child = minimal::client::MakeConfirmationModal(
                        request_copy, theme,
                        [app, session_id, request_copy](const std::string& chosen) {
                            app->AnswerConfirmation(session_id, request_copy.request_id, chosen);
                        });
                }
                show_confirmation = true;
            } else {
                show_confirmation = false;
            }
        } else {
            show_permission = false;
            show_confirmation = false;
            Elements start = render_start_views("");
            if (start.empty()) {
                rows.push_back(vbox({text("minimal") | bold,
                                     text("ctrl+o opens the session picker; typing starts a "
                                          "session") |
                                         dim}) |
                               center | flex);
            } else {
                for (auto& element : start) rows.push_back(std::move(element) | flex);
            }
        }

        if (show_panel) {
            rows.push_back(minimal::client::RenderToolsPanel(
                               tabs, /*stdout_buffer_lines=*/100, theme) |
                           size(HEIGHT, LESS_THAN, panel_height) | yframe);
        }

        rows.push_back(editor->Render() | flex);

        // Status bar. model/agent/permission_mode have no live wire source yet
        // (the metadata emission is E5) and render as nothing -- the format
        // string's unknown-or-unfilled rule.
        std::map<std::string, std::string> values;
        values["version"] = std::string(minimal::common::Version());
        // The focused tab's DEEP snapshot (see focused_details above) drives
        // the status bar's values.
        if (!focused_details.session_id.empty()) {
            values["session"] = focused_details.display_name;
            // The interrupt press is never silent: from SendCancel until the
            // turn actually ends, the focused session says so (status bar =
            // the home of focused-session detail, per ARCHITECTURE.md).
            if (focused_details.cancel_requested) {
                values["session"] += "  ·  cancelling…";
            }
            // {tokens} is used/max now that the metadata emission carries the
            // window (E5a); max unknown (0) renders used-only, the E4 behavior.
            const int64_t used =
                focused_details.input_tokens + focused_details.output_tokens;
            if (used > 0) {
                values["tokens"] = focused_details.context_window_tokens > 0
                                       ? std::to_string(used) + "/" +
                                             std::to_string(focused_details.context_window_tokens) +
                                             " tok"
                                       : std::to_string(used) + " tok";
            }
            // The metadata segments: empty = unknown = render nothing, the
            // format string's unknown-or-unfilled rule.
            if (!focused_details.model.empty()) values["model"] = focused_details.model;
            if (!focused_details.agent_preset.empty()) values["agent"] = focused_details.agent_preset;
            if (!focused_details.permission_mode.empty()) {
                values["permission_mode"] = focused_details.permission_mode;
            }
        }
        // Registered segment providers fill their own ids. Render runs on the
        // render thread at ~20 Hz -- the provider contract (cheap, never
        // blocking) is stated on the interface; a THROW is contained (that
        // segment renders nothing for this frame), never fatal.
        for (const auto& segment : status_registry.segments()) {
            try {
                values[std::string(segment->id())] = segment->Render();
            } catch (const std::exception& error) {
                minimal::common::DefaultLogger().error("status segment '{}' threw: {}",
                                              std::string(segment->id()), error.what());
            } catch (...) {
                minimal::common::DefaultLogger().error("status segment '{}' threw a non-standard "
                                                       "exception", std::string(segment->id()));
            }
        }
        rows.push_back(minimal::client::RenderStatusBar(status_format, values, theme));

        // Plugin PANEL views, beside the tools panel. Events reach the visible
        // ones through the frame's CatchEvent (see with_keys). A throwing
        // view is hidden for the frame that threw (never fatal), like the
        // status segments.
        for (auto& entry : plugin_views) {
            if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible) {
                try {
                    rows.push_back(entry.component->Render() |
                                   size(HEIGHT, LESS_THAN, panel_height) | yframe);
                } catch (const std::exception& error) {
                    minimal::common::DefaultLogger().error("plugin view '{}' threw: {}",
                                                           std::string(entry.view->spec().id),
                                                           error.what());
                } catch (...) {
                    minimal::common::DefaultLogger().error(
                        "plugin view '{}' threw a non-standard exception",
                        std::string(entry.view->spec().id));
                }
            }
        }

        return vbox(std::move(rows));
    });

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
        show_picker, show_agent_picker, show_panel, show_plugin_modal,
        plugin_views, plugin_modal_child,
        open_picker, open_agent_picker, request_quit, close_tab,
        // ChatView::ScrollBy, if the tab has one yet (ui.chat is built
        // lazily inside session_view's own render lambda, so it may not
        // exist on the very first frame of a freshly opened tab).
        [&](const std::string& session_id, int delta_rows) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollBy(delta_rows);
            }
        },
        // tab.close with work in flight: stage ONE confirmation for the
        // whole closing group, same as request_quit's own busy branch.
        [&](std::vector<std::string> ids) {
            pending_close = PendingClose{};
            pending_close.session_ids = std::move(ids);
            show_quit_confirm = true;
        }};
    auto with_keys = CatchEvent(session_view, [&](const Event& event) {
        return minimal::client::HandleGlobalKey(key_ctx, event);
    });

    // Modal stack, OUTERMOST = TOPMOST: the security UI (permission prompt)
    // above the wire confirmation above the quit confirmation above the picker
    // above the delete confirmation above the plugin modals above the frame
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
    auto with_quit_confirm = Modal(tree, confirm_modal, &show_quit_confirm);
    auto with_confirmation = Modal(with_quit_confirm, confirmation_host_interactive,
                                   &show_confirmation);
    auto with_permission = Modal(with_confirmation, permission_host_interactive, &show_permission);

    // The maintenance pass rides the event path ABOVE the whole stack: this
    // handler sees every event the loop processes (including the pump's
    // Event::Custom, even while a modal would consume ordinary keys), runs
    // run_maintenance, and consumes NOTHING -- a pure pre-render observation
    // point. This is what keeps side-effects out of Render().
    Component final_tree = CatchEvent(with_permission, [&](const Event&) {
        run_maintenance();
        return false;
    });
    run_maintenance();   // the first frame must not render an empty snapshot

    // THE PUMP: reader threads wake nothing by themselves; this loop redraws
    // at up to 20 Hz from snapshots. Redraws are gated on the app's UI
    // revision -- an idle TUI used to repaint the full frame 20x per second
    // no matter what. A 5 Hz floor remains so the editor's @-mention debounce
    // gate (which fires FROM a render pass) is serviced while the user types.
    std::atomic<bool> running{true};
    std::thread pump([&] {
        uint64_t last_version = app->UiVersion();
        int tick = 0;
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++tick;
            const uint64_t version = app->UiVersion();
            if (version != last_version || tick % 4 == 0) {
                last_version = version;
                screen.PostEvent(Event::Custom);
            }
        }
    });

    screen.Loop(final_tree);
    running.store(false, std::memory_order_release);
    pump.join();

    // Join the async RPC worker BEFORE tearing down tabs: a job's completion
    // (RequestScrollback et al.) publishes into SessionState/AppState the
    // same way a reader thread's dispatch does, and DetachAllTabs() below
    // destroys the very tabs a straggling job would publish into.
    app->StopRpcQueue();

    // S5.4: quitting the TUI DETACHES every tab rather than terminating it --
    // a session now outlives the client that opened it, so closing the
    // terminal must not destroy in-flight work. ctrl+w (CloseTab) is the
    // explicit per-tab terminate path and is unchanged.
    app->DetachAllTabs();
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
            return RunHeadless(parsed.headless);
        case minimal::client::ParsedArgs::Action::Tui:
            break;
    }
    return RunTui(parsed);
}
