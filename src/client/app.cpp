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
#include "minimal/client/key_event_parser.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/keybindings.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/client/plugins/client_plugin_loader.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/client/render/color_downsample.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/name_registry.hpp"

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
using minimal::client::SessionState;
using minimal::client::SessionStateKind;
using minimal::client::Theme;

// A real modifier+key parser (src/client/key_event_parser.{hpp,cpp}) now
// backs this -- it used to be a closed if-ladder over twelve literal key
// names, so any other keybindings.json binding (ctrl+p, f2, alt+j) matched
// nothing despite the startup warning naming the gap rather than closing it.
std::vector<ftxui::Event> KeyToEvents(const std::string& key) {
    return minimal::client::ParseKeySpec(key);
}

bool MatchesAny(const ftxui::Event& event, const std::vector<ftxui::Event>& candidates) {
    for (const auto& candidate : candidates) {
        if (event == candidate) return true;
    }
    return false;
}

constexpr const char* kDefaultStatusFormat =
    "{version} | {session} | {tokens} | {model} | {agent} | {permission_mode}";

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
    //
    // ORDER MATTERS, and each step is load-bearing for a later one:
    //   1. ui.json (status format -- it selects plugin segments by id)
    //   2. vendored grammars registered as the additive registry's defaults
    //   3. PLUGIN LOAD (contexts carry the attention sink and the data path;
    //      registries record what each plugin claimed)
    //   4. NameRegistry resolve: collisions fatal, dangling aliases warned
    //   5. THEME (sections may reference loaded plugin names)
    //   6. keybindings (+ plugin view defaults, only where nothing binds)
    //   7. core defaults fill every unclaimed slot, then the frame is assembled
    const auto config_dir = minimal::common::ConfigDir();
    const auto depth = minimal::client::DetectColorDepth();
    const auto ui = minimal::client::LoadUiConfig(config_dir);

    plugins::AttentionState attention_state;
    // The seam objects must OUTLIVE plugin registration: a plugin captures the
    // raw pointer from its context at registration and may call it for the
    // process lifetime (lazy Find, attention raises). Binding the factories
    // inline into the loader call destroyed the temporaries at the end of that
    // full expression -- every seam call was a virtual dispatch through freed
    // memory.
    const auto attention_sink = plugins::MakeHostAttentionSink(&attention_state);
    const auto host_client_api = plugins::MakeHostClientApi(client.get());
    plugins::ViewRegistry view_registry;
    plugins::ToolResultRendererRegistry renderer_registry;
    plugins::MessageRendererRegistry message_registry;
    plugins::StatusSegmentRegistry status_registry;
    plugins::SyntaxGrammarRegistry grammar_registry;
    plugins::ChatSurfaceRegistry surface_registry;
    plugins::MarkdownEngineRegistry engine_registry;

    plugins::RegisterCoreSyntaxGrammars(grammar_registry);

    // Discovery + load. The config file is the client's own (client-plugins.json);
    // the server owns plugins.json, so neither writer can stomp the other's
    // entries. Written back after discovery, like the server's.
    plugins::ClientPluginConfig plugin_config;
    {
        const auto config_path = config_dir / "client-plugins.json";
        std::error_code ec;
        if (std::filesystem::exists(config_path, ec)) {
            std::ifstream in(config_path);
            const std::string contents((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            plugin_config = plugins::ParseClientPluginConfig(contents);
            // ParseClientPluginConfig degrades a malformed file to an empty
            // config; silently rewriting the user's bytes over that default
            // would destroy their enablement decisions and per-plugin config
            // (a trailing comma would disable every user plugin on next
            // boot). On a malformed parse: warn and DO NOT rewrite.
            if (plugin_config.malformed) {
                plugin_config.preserved = contents;
                std::fprintf(stderr,
                             "minimal: warning: %s is malformed -- it was NOT rewritten; "
                             "fix or remove it to enable plugin config management\n",
                             std::filesystem::path(config_path).c_str());
            }
        }
        const auto candidates = plugins::DiscoverClientPlugins(
            minimal::common::BundledClientPluginDir(), config_dir / "client-plugins",
            plugin_config);
        if (!plugin_config.malformed) {
            // Same discipline as the server's WriteConfigIfChanged: create the
            // parent dir, skip the write when nothing changed, report errors.
            std::filesystem::create_directories(config_dir, ec);
            const auto serialized = plugins::SerializeClientPluginConfig(plugin_config);
            std::string existing;
            if (std::filesystem::exists(config_path, ec)) {
                std::ifstream in(config_path);
                existing = std::string((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            }
            if (existing != serialized) {
                std::ofstream out(config_path);
                out << serialized;
                if (!out) {
                    std::fprintf(stderr, "minimal: warning: could not write %s\n",
                                 std::filesystem::path(config_path).c_str());
                }
            }
        }

        plugins::ClientPluginLoader loader(view_registry, renderer_registry, message_registry,
                                           status_registry, grammar_registry, surface_registry,
                                           engine_registry, attention_sink.get(),
                                           host_client_api.get());
        const auto records = loader.LoadAll(candidates);

        // Provenance-keyed severity, mirrored from the server's sanity check:
        // a failed BUNDLED plugin is fatal (a silently missing renderer reads
        // as a bug), a failed user plugin is a warning.
        bool fatal = false;
        for (const auto& record : records) {
            if (record.ok() || record.status == plugins::LoadStatus::Disabled) {
                if (record.status == plugins::LoadStatus::Disabled) continue;
                std::fprintf(stderr, "minimal: client plugin '%s' loaded (%s)\n",
                             record.name.c_str(), record.version.c_str());
                continue;
            }
            if (record.provenance == plugins::Provenance::Bundled) {
                std::fprintf(stderr, "minimal: FATAL: bundled client plugin '%s': %s (%s)\n",
                             record.file.string().c_str(), plugins::ToString(record.status).data(),
                             record.error.c_str());
                fatal = true;
            } else {
                std::fprintf(stderr, "minimal: warning: client plugin '%s': %s (%s)\n",
                             record.file.string().c_str(), plugins::ToString(record.status).data(),
                             record.error.c_str());
            }
        }
        if (fatal) return kUnavailable;

        // Name collisions: the SAME rule as the server, through the shared
        // NameRegistry (moved to common so both loaders run one mechanism).
        // Registration keys are original names; a duplicate surface key is
        // fatal and names both claimants.
        minimal::plugins::NameRegistry names;
        const auto feed = [&names](minimal::plugins::NameKind kind,
                                   const std::vector<plugins::Attribution>& attributions) {
            for (const auto& attribution : attributions) {
                names.Register(kind, attribution.plugin_name, attribution.original_name);
            }
        };
        feed(minimal::plugins::NameKind::ViewId, view_registry.attributions());
        feed(minimal::plugins::NameKind::ToolResultRenderer, renderer_registry.attributions());
        for (const auto& [kind, attribution] : message_registry.claims()) {
            names.Register(minimal::plugins::NameKind::MessageRenderer,
                           attribution.plugin_name, attribution.original_name);
        }
        feed(minimal::plugins::NameKind::StatusSegment, status_registry.attributions());
        feed(minimal::plugins::NameKind::SyntaxGrammar, grammar_registry.attributions());
        // Client aliases resolve collisions (the ARCHITECTURE.md rule: a
        // duplicate view id / tool name is "fatal and alias-resolvable"). They
        // were parsed, serialized and documented while never reaching the
        // NameRegistry -- so the alias key did nothing and the dangling-alias
        // diagnostic below was unreachable. Like the server's resolver: the
        // alias lands on the ONE kind whose stable identity it names (so a
        // resolved alias renames that entry's surface and dedupes the
        // collision), and an identity no kind claims is dangling on a single
        // kind -- reported once, not four times. Addressing stays CANONICAL
        // (keybindings.json addresses ids, never aliases).
        const auto alias_on_kind = [&](const std::string& key, const std::string& target) {
            const auto matches = [](const std::vector<plugins::Attribution>& attributions,
                                    const std::string& qualified) {
                for (const auto& a : attributions) {
                    if (a.plugin_name + "." + a.original_name == qualified) return true;
                }
                return false;
            };
            if (matches(view_registry.attributions(), key)) {
                names.SetAlias(minimal::plugins::NameKind::ViewId, key, target);
            } else if (matches(renderer_registry.attributions(), key)) {
                names.SetAlias(minimal::plugins::NameKind::ToolResultRenderer, key, target);
            } else {
                bool matched_message = false;
                for (const auto& [kind, attribution] : message_registry.claims()) {
                    (void)kind;
                    if (attribution.plugin_name + "." + attribution.original_name == key) {
                        names.SetAlias(minimal::plugins::NameKind::MessageRenderer, key, target);
                        matched_message = true;
                    }
                }
                if (!matched_message) {
                    if (matches(status_registry.attributions(), key)) {
                        names.SetAlias(minimal::plugins::NameKind::StatusSegment, key, target);
                    } else if (matches(grammar_registry.attributions(), key)) {
                        names.SetAlias(minimal::plugins::NameKind::SyntaxGrammar, key, target);
                    } else {
                        names.SetAlias(minimal::plugins::NameKind::ViewId, key, target);   // dangling
                    }
                }
            }
        };
        for (const auto& [alias_key, alias_target] : plugin_config.aliases) {
            alias_on_kind(alias_key, alias_target);
        }
        const auto resolved = names.Resolve();
        for (const auto& collision : resolved.fatal_collisions) {
            std::fprintf(stderr, "minimal: FATAL: client plugin name collision: %s\n",
                         collision.c_str());
            fatal = true;
        }
        for (const auto& dangling : resolved.dangling_aliases) {
            std::fprintf(stderr, "minimal: warning: client plugin alias names nothing: %s\n",
                         dangling.c_str());
        }
        if (fatal) return kUnavailable;
    }

    // Loaded plugin names feed the theme's section check.
    std::vector<std::string> loaded_plugins;
    for (const auto& attribution : view_registry.attributions()) {
        if (!attribution.plugin_name.empty()) loaded_plugins.push_back(attribution.plugin_name);
    }
    for (const auto& attribution : renderer_registry.attributions()) {
        if (!attribution.plugin_name.empty()) loaded_plugins.push_back(attribution.plugin_name);
    }
    for (const auto& attribution : status_registry.attributions()) {
        if (!attribution.plugin_name.empty()) loaded_plugins.push_back(attribution.plugin_name);
    }
    for (const auto& attribution : grammar_registry.attributions()) {
        if (!attribution.plugin_name.empty()) loaded_plugins.push_back(attribution.plugin_name);
    }

    // ui.json's "theme" selects the file (themes/<name>.json); empty means
    // the built-in default. Previously hardcoded to "dark" regardless of
    // config, which made the bundled light.json -- and any user theme --
    // unreachable from the TUI.
    const std::string theme_name = !ui.theme.empty() ? ui.theme : std::string("dark");
    Theme theme = [&] {
        try {
            return Theme::Load(config_dir / "themes", theme_name, depth, &loaded_plugins);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using terminal defaults\n", e.what());
            // An empty-but-valid document: "no colors set" is the fallback,
            // and ColorFor's Default fallback covers every role. ("{}" is NOT
            // valid here -- Parse requires a "colors" object, so a missing
            // themes dir used to throw out of the fallback itself.)
            return Theme::Parse(R"({"name":"default","colors":{}})", depth);
        }
    }();    Keybindings keys = [&] {
        try {
            return Keybindings::Load(config_dir);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using default keybindings\n", e.what());
            return Keybindings::Parse("{}");
        }
    }();

    // Plugin views: register each id's suggested default keybinding only where
    // nothing binds it yet (the user's file wins), and remember the runtime
    // dispatch entries.
    struct PluginViewEntry {
        const plugins::View* view;
        ftxui::Component component;
        bool visible = false;
    };
    std::vector<PluginViewEntry> plugin_views;
    for (const auto& view : view_registry.views()) {
        keys.EnsureDefault(view->spec().id, view->spec().default_keybinding);
        plugin_views.push_back(PluginViewEntry{view.get(), view->component(), false});
    }
    for (const auto& [action, key] : keys.all()) {
        if (KeyToEvents(key).empty()) {
            std::fprintf(stderr, "minimal: no key mapping for '%s' -> '%s'\n", action.c_str(),
                         key.c_str());
        }
    }

    // The status format: ui.json's value, or the built-in default. Unknown ids
    // warn UNLESS a registered segment answers to them -- the registry is part
    // of the known-id set now.
    const std::string status_format_string =
        !ui.status_format.empty() ? ui.status_format : std::string(kDefaultStatusFormat);
    const auto status_format = minimal::client::ParseStatusFormat(status_format_string);
    for (const auto& id : minimal::client::UnknownStatusSegments(status_format_string)) {
        if (status_registry.Find(id) == nullptr) {
            std::fprintf(stderr, "minimal: unknown status segment '{%s}'\n", id.c_str());
        }
    }

    // The highlighter consults the assembled grammar table (vendored defaults
    // plus plugin additions), so a plugin's language highlights like a vendored
    // one. The renderer owns it; the engine routes fenced code through it.
    const std::vector<minimal::client::LanguageDefinition> languages = [&] {
        std::vector<minimal::client::LanguageDefinition> out;
        for (const auto& grammar : grammar_registry.grammars()) {
            out.push_back(minimal::client::LanguageDefinition{
                grammar->Language(), grammar->LanguageFn(), grammar->Rules()});
        }
        return out;
    }();
    minimal::client::SyntaxHighlighter highlighter(theme, languages);

    // Core defaults fill every slot no plugin claimed. The engine first (the
    // agent-markdown default renders through it), then the kind defaults.
    // ONE construction of the renderer, AFTER the highlighter exists: the
    // engine binds a reference to this variable, and the first draft reassigned
    // the variable in place after registering the engine (second constructor
    // call) -- correct only by coincidence of identity, and a refactor that
    // introduced a second variable would dangle the engine silently.
    minimal::client::MarkdownRenderer renderer(&theme, &highlighter);
    plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
    // Non-null by construction: RegisterDefaultMarkdownEngine registers the
    // md4c adapter when no plugin claimed the slot.
    plugins::MarkdownEngine& engine = *engine_registry.engine;
    plugins::RegisterCoreMessageRenderers(message_registry);
    plugins::RegisterCoreAgentMarkdownRenderer(message_registry, engine);

    // The exclusive chat surface, when a plugin claimed it.
    plugins::ChatSurface* chat_surface = surface_registry.surface.get();
    if (chat_surface != nullptr) {
        // A throwing surface is disabled, not fatal (catch + disable, track
        // R2): OnTheme runs before any event flows, so a throw here is the
        // plugin's first chance to kill the process unguarded.
        try {
            chat_surface->OnTheme(theme);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "minimal: chat surface plugin threw on theme (%s) -- "
                                 "disabled; the core chat view takes over\n",
                         error.what());
            chat_surface = nullptr;
        } catch (...) {
            std::fprintf(stderr, "minimal: chat surface plugin threw on theme (non-standard "
                                 "exception) -- disabled; the core chat view takes over\n");
            chat_surface = nullptr;
        }
    }

    auto app = std::make_shared<AppState>(client);
    app->SetPluginAttention(&attention_state);
    app->SetChatSurface(chat_surface);
    // Memory bounds (track R2): ui.json's values, or the built-in defaults
    // (SetUiBounds treats 0 as "keep the default"). Applied before any tab can
    // open, so every SessionState starts bounded.
    app->SetUiBounds(ui.scrollback_window_max_messages, ui.tool_output_buffer_bytes);
    auto screen = ftxui::ScreenInteractive::TerminalOutput();
    auto quit = screen.ExitLoopClosure();

    // --- frame state (loop thread only) --------------------------------------
    PromptEditorState prompt_state;
    bool show_panel = false;
    bool show_picker = false;
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
        // T2: an async RequestScrollback(newest=true) is outstanding for this
        // tab. refresh_seq_seen/refresh_from_version are the pre-submit
        // values (SessionState::Snapshot fields) needed to tell, once
        // SessionState::refresh_seq moves, whether the attempt actually
        // applied a page (transcript_version moved past refresh_from_version)
        // or failed (it didn't) -- see maintain() below.
        bool refresh_pending = false;
        uint64_t refresh_seq_seen = 0;
        uint64_t refresh_from_version = 0;
    };
    std::map<std::string, TabUi> tab_ui;

    std::string focused;   // active session id, refreshed every frame

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

        // Resolve any outstanding refresh BEFORE deciding whether to submit a
        // new one: refresh_seq (bumped by SessionState on every RESOLVED
        // attempt -- success or failure) is a different counter from
        // transcript_version specifically so a failed fetch still tells this
        // frame "stop waiting", without looking identical to "still in
        // flight". This is the fix for the retry bug the old code's own
        // comment named but did not implement ("the next trigger retries" --
        // it did not, because rendered_version was latched before the RPC's
        // outcome was known).
        if (ui.refresh_pending && snap.refresh_seq != ui.refresh_seq_seen) {
            ui.refresh_pending = false;
            if (snap.transcript_version != ui.refresh_from_version) {
                // A page applied: this snapshot was taken after the worker's
                // BumpUiVersion woke the pump, so it already reflects it.
                ui.rendered_version = snap.transcript_version;
            }
            // Else the fetch failed (or the applied page happened to leave
            // transcript_version where it was, e.g. an empty response with
            // nothing new): rendered_version stays behind on purpose, so the
            // mismatch check below retries on this very frame.
        }

        if (!ui.refresh_pending && ui.rendered_version != snap.transcript_version) {
            if (snap.streaming_text.empty()) {
                ui.refresh_pending = true;
                ui.refresh_seq_seen = snap.refresh_seq;
                ui.refresh_from_version = snap.transcript_version;
                app->RequestScrollback(session_id, /*newest=*/true);
            } else {
                ui.rendered_version = snap.transcript_version;
            }
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
    auto editor = minimal::client::MakePromptEditor(
        theme, prompt_state,
        submit_chat,
        [app](const std::string& line) { app->SendCommand(app->FocusedSessionId(), line); },
        suggest,
        [&command_catalog] { return command_catalog; });

    // --- permission modal (inner swapped per outstanding request) -------------
    // The host is a STABLE component in the tree; its inner component is
    // rebuilt when a new request_id arrives. Checkbox state lives in
    // GrantToggles (keyed by request in tab_ui), so it survives the rebuild.
    auto permission_child = std::make_shared<Component>();
    auto permission_host = Renderer([=] {
        return *permission_child ? (*permission_child)->Render() : emptyElement();
    });
    auto permission_host_interactive = CatchEvent(
        permission_host, [=](const Event& event) {
            return show_permission && *permission_child ? (*permission_child)->OnEvent(event)
                                                        : false;
        });

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
    auto confirmation_host = Renderer([=] {
        return *confirmation_child ? (*confirmation_child)->Render() : emptyElement();
    });
    auto confirmation_host_interactive = CatchEvent(
        confirmation_host, [=](const Event& event) {
            return show_confirmation && *confirmation_child
                       ? (*confirmation_child)->OnEvent(event)
                       : false;
        });

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
    auto picker_host = Renderer([=] {
        return *picker_child ? (*picker_child)->Render() : emptyElement();
    });
    auto picker_host_interactive = CatchEvent(picker_host, [=](const Event& event) {
        return show_picker && *picker_child ? (*picker_child)->OnEvent(event) : false;
    });

    // T8: typing on an empty app already starts a session directly
    // (submit_chat, above); the picker's "+ new session" row is the same
    // action reached from inside the modal, with no text to send yet.
    auto open_new_session = [&] {
        show_picker = false;
        app->RunOnRpcQueue([app] {
            minimal::client::SessionClient::CreateOptions opts;
            opts.project_root = std::filesystem::current_path().string();
            app->OpenTab(opts);
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

    // --- plugin views (panel region + modal layer) -----------------------------
    // Panels: rendered in the frame beside the tools panel; one shared
    // CatchEvent (over the frame) routes events to visible ones. Modals: one
    // host in the modal stack BELOW the quit confirmation -- a plugin modal
    // can never preempt the security UI, which is the invariant the nesting
    // itself enforces (see the modal stack below).
    auto plugin_modal_child = std::make_shared<Component>();
    auto plugin_modal_host = Renderer([=] {
        if (!*plugin_modal_child) return emptyElement();
        // A throwing plugin modal hides itself for the frame (never fatal).
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
                if (const auto status = client->DeleteSession(staged_delete_id); !status.ok()) {
                    std::fprintf(stderr, "minimal: delete failed: %s\n",
                                 status.error_message().c_str());
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

    auto session_view = Renderer(editor, [&] {
        using namespace ftxui;

        const auto tabs = app->GetAllTabs();
        focused.clear();
        for (const auto& tab : tabs) {
            if (tab.is_focused) focused = tab.session_id;
        }

        // ONE deep snapshot of the focused tab per frame: GetDetails is a full
        // protobuf copy of the scrollback window (up to
        // scrollback_window_max_messages rows), and this used to be paid up
        // to three times per frame -- once here, again inside maintain, again
        // inside handle_notice. maintain() may replace it with a fresher one
        // if a scrollback refresh fires (it must -- the refresh mutates
        // SessionState out from under the copy this call started with);
        // handle_notice and the renderer below both read whatever maintain
        // settled on.
        auto focused_details = app->GetDetails(focused);
        if (!focused.empty()) {
            focused_details = maintain(focused, std::move(focused_details));
            handle_notice(focused_details);   // once-per-notice VIEW side-effects (never in Render)
        }

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
                if (!ui.chat) {
                    ui.chat = std::make_unique<minimal::client::ChatView>(
                        theme, message_registry, renderer_registry);
                }
                rows.push_back(ui.chat->Render(snap, *ui.streaming) | yframe |
                               vscroll_indicator | flex);
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
            rows.push_back(vbox({text("minimal") | bold,
                                 text("ctrl+o opens the session picker; typing starts a session") |
                                     dim}) |
                            center | flex);
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
    auto with_keys = CatchEvent(session_view, [&](const Event& event) {
        const std::string sid = focused;

        if (MatchesAny(event, KeyToEvents(keys.KeyFor("tab.next")))) {
            app->CycleFocusForward();
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("tab.previous")))) {
            app->CycleFocusBackward();
            return true;
        }
        const bool group_next = MatchesAny(event, KeyToEvents(keys.KeyFor("group.next")));
        const bool group_prev = MatchesAny(event, KeyToEvents(keys.KeyFor("group.previous")));
        if (group_next || group_prev) {
            // Between root groups: focus the next (or previous) ROOT tab.
            // The index math is pulled out to keybinding_dispatch.hpp and
            // unit-tested there -- this is the same shape of "find my index
            // among a filtered subset" bug that jump.attention had (below).
            const auto tabs = app->GetAllTabs();
            const auto target = NextRootGroup(tabs, sid, group_next);
            if (!target.empty()) app->SetFocusedTab(target);
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("jump.attention")))) {
            // Crosses groups; wraps. Only Waiting tabs qualify (the ! glyph).
            // Scans starting just AFTER the focused tab -- not a fixed index,
            // which ignored which tab was actually focused and could "jump"
            // to the focused tab itself, a visible no-op exactly when
            // Waiting is the reason someone pressed the key. See
            // keybinding_dispatch.hpp/NextAttentionTab, which this bug is
            // now a unit test of.
            const auto target = NextAttentionTab(app->GetAllTabs(), sid);
            if (!target.empty()) app->SetFocusedTab(target);
            return true;
        }
        // INTERRUPT -- the interactive loop's missing verb. Escape and ctrl+c
        // on a busy tab send CancelTurn; the server ends the turn with
        // STOP_REASON_CANCELLED and answers any prompt it was parked on with
        // a denial (pending.CancelAll), so a wedged prompt is interruptible
        // too. Waiting counts as busy. Modals sit OUTERMOST in the stack, so
        // while one is up it consumes these keys first (permission:
        // esc = deny; confirmation: esc = a cancel option when offered) --
        // this branch only fires with no modal shown. The busy probe runs
        // only when one of the two keys actually arrived, never per keystroke.
        const bool cancel_pressed =
            event == Event::CtrlC ||
            MatchesAny(event, KeyToEvents(keys.KeyFor("turn.cancel")));
        if (cancel_pressed) {
            bool busy = false;
            if (!sid.empty()) {
                for (const auto& tab : app->GetAllTabs()) {
                    if (tab.session_id != sid) continue;
                    switch (tab.state) {
                        case SessionStateKind::Thinking:
                        case SessionStateKind::Working:
                        case SessionStateKind::Waiting:
                            busy = true;
                            break;
                        default:
                            break;
                    }
                    break;
                }
            }
            if (busy) {
                app->SendCancel(sid);
                return true;
            }
            // Idle ctrl+c means quit: the universal exit reflex, otherwise
            // DEAD (raw mode -- no SIGINT ever reaches this process). Idle
            // Escape falls through to the editor, which has no use for it.
            if (event == Event::CtrlC) {
                request_quit();
                return true;
            }
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("session.picker")))) {
            show_picker = !show_picker;
            if (show_picker) open_picker();
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("panel.toggle")))) {
            show_panel = !show_panel;
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("chat.load_older")))) {
            if (!sid.empty()) app->RequestScrollback(sid, /*newest=*/false);
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("tab.close")))) {
            if (!sid.empty()) {
                // Closing a ROOT tab terminates its group: stage children
                // first, the root last (TabsToClose, keybinding_dispatch.hpp).
                // ONE confirmation if any of it is in flight.
                const auto tabs = app->GetAllTabs();
                PendingClose staged;
                staged.session_ids = TabsToClose(tabs, sid);
                if (AnyWorkInFlight(tabs, staged.session_ids)) {
                    pending_close = std::move(staged);   // ONE confirmation for the group
                    show_quit_confirm = true;
                } else {
                    for (const auto& id : staged.session_ids) close_tab(id);
                }
            }
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("app.quit")))) {
            request_quit();
            return true;
        }

        // Plugin views, after every core action: a visible PANEL gets the
        // event first (its component consumes what it wants); any view id
        // bound in keybindings.json toggles its visibility. The ids were
        // registered as runtime defaults (EnsureDefault), so a user's file
        // that already names them wins.
        for (auto& entry : plugin_views) {
            if (entry.view->spec().placement == plugins::ViewPlacement::Panel && entry.visible &&
                entry.component->OnEvent(event)) {
                return true;
            }
        }
        for (auto& entry : plugin_views) {
            if (MatchesAny(event, KeyToEvents(keys.KeyFor(entry.view->spec().id)))) {
                entry.visible = !entry.visible;
                if (entry.view->spec().placement == plugins::ViewPlacement::Modal) {
                    *plugin_modal_child = entry.component;
                    show_plugin_modal = entry.visible;
                }
                return true;
            }
        }
        return false;
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
    auto with_quit_confirm = Modal(tree, confirm_modal, &show_quit_confirm);
    auto with_confirmation = Modal(with_quit_confirm, confirmation_host_interactive,
                                   &show_confirmation);
    auto with_permission = Modal(with_confirmation, permission_host_interactive, &show_permission);
    Component final_tree = with_permission;

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
    // same way a reader thread's dispatch does, and CloseAllTabs() below
    // destroys the very tabs a straggling job would publish into.
    app->StopRpcQueue();

    // Closing the TUI closes every tab: terminate all attached sessions
    // (ARCHITECTURE.md "Session lifetime" -- nothing outlives its client).
    app->CloseAllTabs();
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
