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
#include "minimal/client/view/frame_renderer.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/picker_controller.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tab_ui.hpp"
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
    opts.project_root = std::filesystem::current_path().string();
    return opts;
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
using minimal::client::TabUi;
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
    PendingClose pending_close;

    // Per-tab UI caches, keyed by session id. TabUi's type and the
    // maintenance logic that reads/writes it both live in
    // view/tab_ui.{hpp,cpp}; the MAP stays a plain RunTui local (not owned
    // by TabMaintenance) because close_tab (below) erases from it directly,
    // and close_tab is defined before TabMaintenance can be constructed --
    // it needs open_picker/plugin_modal_child, built later.
    std::map<std::string, minimal::client::TabUi> tab_ui;

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
    auto submit_chat = [app, &tab_ui](const std::string& text) {
        const std::string sid = app->FocusedSessionId();
        if (sid.empty()) {
            app->RunOnRpcQueue([app, text] {
                const auto opts = DefaultCreateOptions();
                if (const auto new_sid = app->OpenTab(opts); !new_sid.empty()) {
                    app->SendChatTurn(new_sid, text);
                }
                // A failed create leaves the app empty; the user sees no new
                // tab and can retry, same visible outcome the synchronous
                // call had on failure.
            });
            return;
        }
        // Sending a turn returns the view to the newest row: scrolled-up
        // content would otherwise sit frozen while the reply streams in
        // somewhere below the visible viewport, reading as if nothing
        // happened.
        if (auto it = tab_ui.find(sid); it != tab_ui.end() && it->second.chat) {
            it->second.chat->ScrollToBottom();
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

    // --- agent preset picker ---------------------------------------------------
    auto agent_picker_child = std::make_shared<Component>();
    auto agent_picker_host_interactive =
        minimal::client::MakeModalHost(agent_picker_child, show_agent_picker);

    // T8: typing on an empty app already starts a session directly
    // (submit_chat, above); the picker's "+ new session" row is the same
    // action reached from inside the modal, with no text to send yet.
    auto open_new_session = [&] {
        show_picker = false;
        app->RunOnRpcQueue([app] {
            const auto opts = DefaultCreateOptions();
            if (const auto sid = app->OpenTab(opts); !sid.empty()) app->SetFocusedTab(sid);
        });
    };

    // Owns picker_data/agent_picker_presets and the fetch/rebuild/open
    // flows privately; writes rebuilt components into picker_child/
    // agent_picker_child (declared above -- the modal slots RunTui's own
    // frame assembly and MakeModalHost calls read from, unchanged).
    minimal::client::PickerController picker_controller(
        minimal::client::PickerController::Dependencies{
            *client, *app, theme, picker_child, agent_picker_child, show_picker,
            show_agent_picker, staged_delete_id, show_delete_confirm, open_new_session});
    auto open_picker = [&] { picker_controller.OpenSessionPicker(); };
    auto open_agent_picker = [&] { picker_controller.OpenAgentPicker(); };

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
        *app, engine, chat_surface, view_registry, plugin_views, plugin_modal_child,
        show_plugin_modal, open_picker, open_agent_picker, tab_ui});
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
        const auto opts = DefaultCreateOptions();
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

    // FrameRenderer owns the tab body / permission-confirmation-modal /
    // status-bar / plugin-panel assembly; constructed once, referencing
    // RunTui's locals the same way TabMaintenance/PickerController do.
    // `frame_renderer` itself must never be moved/copied for the rest of
    // RunTui's lifetime (its Dependencies hold references into RunTui's own
    // objects, resolved once at construction).
    minimal::client::FrameRenderer frame_renderer(minimal::client::FrameRenderer::Dependencies{
        *app, theme, focused, focused_details, tab_ui, start_view_registry, message_registry,
        renderer_registry, status_registry, engine, permission_child, show_permission,
        confirmation_child, show_confirmation, show_panel, panel_height,
        env.ui.tools_panel_stdout_buffer_lines > 0 ? env.ui.tools_panel_stdout_buffer_lines : 100,
        editor, status_format, plugin_views});
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
        open_picker, open_agent_picker, request_quit, close_tab,
        // ChatView::ScrollBy, if the tab has one yet (ui.chat is built
        // lazily inside session_view's own render lambda, so it may not
        // exist on the very first frame of a freshly opened tab).
        [&](const std::string& session_id, int delta_rows) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollBy(delta_rows);
            }
        },
        [&](const std::string& session_id) {
            if (auto it = tab_ui.find(session_id); it != tab_ui.end() && it->second.chat) {
                it->second.chat->ScrollToBottom();
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
    // the maintenance pass, and consumes NOTHING -- a pure pre-render
    // observation point. This is what keeps side-effects out of Render().
    Component final_tree = CatchEvent(with_permission, [&](const Event&) {
        tab_maintenance.Run();
        return false;
    });
    tab_maintenance.Run();   // the first frame must not render an empty snapshot

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
