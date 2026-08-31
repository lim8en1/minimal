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
#include "minimal/client/keybindings.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/client/render/color_downsample.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
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
    if (!options.resume_session_id.empty()) {
        // Resume: the stored session keeps its preset and project_root; the
        // server refuses them rather than silently ignoring a conflict.
        create.resume_session_id = options.resume_session_id;
    } else {
        create.project_root = std::filesystem::current_path().string();
        create.agent_preset = options.agent_preset;
        create.name = options.prompt.substr(0, 60);
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

// --- TUI --------------------------------------------------------------------
//
// Frame assembly (ARCHITECTURE.md "TUI layout"): tab bar / session view /
// optional running-tools panel / status bar, plus a modal layer.
//
// THE BRIDGE: gRPC reader threads never touch FTXUI objects. They mutate
// SessionState/AppState (plain, mutex-guarded state), and a 20 Hz pump posts a
// custom event into the screen loop so it redraws from a snapshot. Every
// component below READS state; nothing on the loop thread waits on the reader,
// and nothing on the reader waits on the loop. The one exception is
// RefreshScrollback -- a bounded (2 s deadline) unary call made ON the loop
// thread at a fetch moment, chosen over detached threads whose lifetime would
// race the screen's teardown (see AppState::RefreshScrollback).
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

std::vector<ftxui::Event> KeyToEvents(const std::string& key) {
    using namespace ftxui;
    if (key == "ctrl+tab") return {Event::Character("\x1b[27;5;9~")};
    if (key == "ctrl+shift+tab") return {Event::Character("\x1b[27;6;9~")};
    if (key == "alt+left") return {Event::Character("\x1b[1;3D"), Event::Character("\x1b\x1b[D")};
    if (key == "alt+right") return {Event::Character("\x1b[1;3C"), Event::Character("\x1b\x1b[C")};
    if (key == "alt+a") return {Event::AltA};
    if (key == "ctrl+o") return {Event::CtrlO};
    if (key == "ctrl+t") return {Event::CtrlT};
    if (key == "ctrl+w") return {Event::CtrlW};
    if (key == "ctrl+q") return {Event::CtrlQ};
    if (key == "pageup") return {Event::PageUp};
    return {};
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

    // Config. A malformed decoration file is WARNED about and defaults applied,
    // not fatal: a broken theme or keybindings file must not take down the
    // harness (contrast the server's core config files, which are fatal -- the
    // difference is what the file can affect).
    const auto config_dir = minimal::common::ConfigDir();
    const auto depth = minimal::client::DetectColorDepth();
    Theme theme = [&] {
        try {
            return Theme::Load(config_dir / "themes", "dark", depth);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using terminal defaults\n", e.what());
            return Theme::Parse("{}", depth);
        }
    }();
    Keybindings keys = [&] {
        try {
            return Keybindings::Load(config_dir);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "minimal: %s; using default keybindings\n", e.what());
            return Keybindings::Parse("{}");
        }
    }();
    for (const auto& [action, key] : keys.all()) {
        if (KeyToEvents(key).empty()) {
            std::fprintf(stderr, "minimal: no key mapping for '%s' -> '%s'\n", action.c_str(),
                         key.c_str());
        }
    }
    const auto status_format = minimal::client::ParseStatusFormat(kDefaultStatusFormat);
    for (const auto& id : minimal::client::UnknownStatusSegments(kDefaultStatusFormat)) {
        std::fprintf(stderr, "minimal: unknown status segment '{%s}'\n", id.c_str());
    }

    auto app = std::make_shared<AppState>(client);
    auto screen = ftxui::ScreenInteractive::TerminalOutput();
    auto quit = screen.ExitLoopClosure();

    // --- frame state (loop thread only) --------------------------------------
    PromptEditorState prompt_state;
    bool show_panel = false;
    bool show_picker = false;
    bool show_quit_confirm = false;
    bool show_permission = false;
    std::vector<minimal::v1::SessionSummary> picker_sessions;
    PendingClose pending_close;

    // Per-tab UI caches, keyed by session id.
    struct TabUi {
        std::unique_ptr<minimal::client::StreamingMarkdownView> streaming;
        std::size_t streaming_seen = 0;
        std::optional<uint64_t> rendered_version;
        std::unique_ptr<GrantToggles> grants;   // per outstanding request
        std::string grants_for_request;
    };
    std::map<std::string, TabUi> tab_ui;

    minimal::client::MarkdownRenderer renderer(&theme, nullptr);
    minimal::client::ChatView chat_view(theme, renderer);

    std::string focused;   // active session id, refreshed every frame

    auto focused_snapshot = [&]() -> std::optional<SessionState::Snapshot> {
        const auto details = app->GetAllDetails();
        const auto it = details.find(focused);
        return it != details.end() ? std::optional(it->second) : std::nullopt;
    };

    // Per-frame maintenance for the focused tab: feed streaming deltas to the
    // debounced view, and trigger the post-turn scrollback refresh. A turn in
    // flight is NOT fetched around -- the streaming block is the live content
    // until the turn ends and the refresh swaps it for the persisted rows.
    auto maintain = [&](const std::string& session_id) {
        const auto details = app->GetAllDetails();
        const auto it = details.find(session_id);
        if (it == details.end()) return;
        const auto& snap = it->second;
        TabUi& ui = tab_ui[session_id];

        const auto make_stream = [&] {
            return std::make_unique<minimal::client::StreamingMarkdownView>(
                [this_ = &renderer](std::string_view text) { return this_->Render(text); });
        };
        if (ui.streaming == nullptr) ui.streaming = make_stream();

        if (snap.streaming_text.size() > ui.streaming_seen) {
            ui.streaming->AppendDelta(
                std::string_view(snap.streaming_text).substr(ui.streaming_seen));
        } else if (snap.streaming_text.size() < ui.streaming_seen) {
            ui.streaming = make_stream();   // a new turn; the old stream is gone
        }
        ui.streaming_seen = snap.streaming_text.size();

        if (ui.rendered_version != snap.transcript_version) {
            if (snap.streaming_text.empty()) {
                app->RefreshScrollback(session_id, /*newest=*/true);
                const auto fresh = app->GetAllDetails();
                const auto fit = fresh.find(session_id);
                ui.rendered_version =
                    fit != fresh.end() ? std::optional(fit->second.transcript_version)
                                       : std::optional(snap.transcript_version);
            } else {
                ui.rendered_version = snap.transcript_version;
            }
        }
    };

    // --- editor ---------------------------------------------------------------
    auto suggest = [app, client](const std::string& prefix) -> std::vector<std::string> {
        const std::string sid = app->FocusedSessionId();
        if (prefix.empty() || sid.empty()) return {};
        const auto result = client->SuggestPaths(sid, prefix, 20);
        if (!result.status.ok()) return {};
        return {result.response.paths().begin(), result.response.paths().end()};
    };
    auto editor = minimal::client::MakePromptEditor(
        theme, prompt_state,
        [app](const std::string& text) { app->SendChatTurn(app->FocusedSessionId(), text); },
        [app](const std::string& line) { app->SendCommand(app->FocusedSessionId(), line); },
        suggest);

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
            for (const auto& id : pending_close.session_ids) app->CloseTab(id);
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

    // --- frame -----------------------------------------------------------------
    int panel_height = 12;
    auto any_work_in_flight = [&](const std::vector<AppState::TabSnapshot>& tabs,
                                  const std::vector<std::string>& ids) {
        const auto details = app->GetAllDetails();
        for (const auto& id : ids) {
            const auto it = details.find(id);
            if (it == details.end()) continue;
            switch (it->second.state) {
                case SessionStateKind::Thinking:
                case SessionStateKind::Working:
                case SessionStateKind::Waiting:
                    return true;
                default:
                    break;
            }
        }
        (void)tabs;
        return false;
    };

    auto session_view = Renderer(editor, [&] {
        using namespace ftxui;

        const auto tabs = app->GetAllTabs();
        focused.clear();
        for (const auto& tab : tabs) {
            if (tab.is_focused) focused = tab.session_id;
        }
        if (!focused.empty()) maintain(focused);

        Elements rows;
        rows.push_back(minimal::client::RenderTabBar(tabs, theme));

        const auto details = app->GetAllDetails();
        const auto dit = details.find(focused);
        if (dit != details.end()) {
            const auto& snap = dit->second;
            TabUi& ui = tab_ui[focused];
            rows.push_back(chat_view.Render(snap, *ui.streaming) | yframe | vscroll_indicator |
                           flex);

            // The transient notice line: the focused tab's latest /-command
            // outcome. NEVER a chat row (EXTENSIBILITY §4.4).
            if (snap.latest_notice.has_value()) {
                rows.push_back(text(" " + snap.latest_notice->message) |
                               bgcolor(theme.ColorFor("status_bar_bg")) |
                               color(snap.latest_notice->is_error ? theme.ColorFor("error")
                                                                  : theme.ColorFor("system_message")));
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
        } else {
            show_permission = false;
            rows.push_back(vbox({text("minimal") | bold,
                                 text("ctrl+o opens the session picker; typing starts a session") |
                                     dim}) |
                            center | flex);
        }

        if (show_panel) {
            rows.push_back(minimal::client::RenderToolsPanel(
                               tabs, details, /*stdout_buffer_lines=*/100, theme) |
                           size(HEIGHT, LESS_THAN, panel_height) | yframe);
        }

        rows.push_back(editor->Render() | flex);

        // Status bar. model/agent/permission_mode have no live wire source yet
        // (the metadata emission is E5) and render as nothing -- the format
        // string's unknown-or-unfilled rule.
        std::map<std::string, std::string> values;
        values["version"] = std::string(minimal::common::Version());
        if (dit != details.end()) {
            values["session"] = dit->second.display_name;
            const int64_t used = dit->second.input_tokens + dit->second.output_tokens;
            if (used > 0) values["tokens"] = std::to_string(used) + " tok";
        }
        rows.push_back(minimal::client::RenderStatusBar(status_format, values, theme));

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
            const auto tabs = app->GetAllTabs();
            std::vector<std::string> roots;
            for (const auto& tab : tabs) {
                if (!tab.is_subagent) roots.push_back(tab.session_id);
            }
            if (!roots.empty()) {
                std::size_t idx = 0;
                for (std::size_t i = 0; i < roots.size(); ++i) {
                    if (roots[i] == sid) idx = i;
                }
                app->SetFocusedTab(roots[(idx + (group_next ? 1 : roots.size() - 1)) %
                                         roots.size()]);
            }
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("jump.attention")))) {
            // Crosses groups; wraps. Only Waiting tabs qualify (the ! glyph).
            const auto tabs = app->GetAllTabs();
            for (std::size_t i = 0; i < tabs.size(); ++i) {
                const std::size_t idx = (tabs.size() + 1 + i) % tabs.size();   // +1: skip current
                if (tabs[idx].attention_glyph == '!') {
                    app->SetFocusedTab(tabs[idx].session_id);
                    return true;
                }
            }
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("session.picker")))) {
            show_picker = !show_picker;
            if (show_picker) {
                picker_sessions.clear();
                const auto result =
                    client->ListSessions(std::filesystem::current_path().string(), false, 200);
                if (result.status.ok()) {
                    picker_sessions.assign(result.response.sessions().begin(),
                                           result.response.sessions().end());
                }
                *picker_child = minimal::client::MakeSessionPicker(
                    picker_sessions, theme,
                    {.on_resume =
                         [&](const std::string& session_id) {
                             // A PAST session resumes into its own root group;
                             // it is not a child of whatever is focused.
                             app->AttachTab(session_id);
                             show_picker = false;
                         },
                     .on_close = [&] { show_picker = false; }});
            }
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("panel.toggle")))) {
            show_panel = !show_panel;
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("chat.load_older")))) {
            if (!sid.empty()) app->RefreshScrollback(sid, /*newest=*/false);
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("tab.close")))) {
            if (!sid.empty()) {
                // Closing a ROOT tab terminates its group: stage children first,
                // the root last. ONE confirmation if any of it is in flight.
                const auto tabs = app->GetAllTabs();
                std::optional<int64_t> group;
                bool is_root = false;
                for (const auto& tab : tabs) {
                    if (tab.session_id == sid) {
                        group = tab.root_ordinal;
                        is_root = !tab.is_subagent;
                    }
                }
                PendingClose staged;
                for (const auto& tab : tabs) {
                    if (!is_root && tab.session_id != sid) continue;
                    if (is_root && group.has_value() && tab.root_ordinal != *group) continue;
                    staged.session_ids.push_back(tab.session_id);
                }
                if (any_work_in_flight(tabs, staged.session_ids)) {
                    pending_close = std::move(staged);   // ONE confirmation for the group
                    show_quit_confirm = true;
                } else {
                    for (const auto& id : staged.session_ids) app->CloseTab(id);
                }
            }
            return true;
        }
        if (MatchesAny(event, KeyToEvents(keys.KeyFor("app.quit")))) {
            // Quit = close every tab, with ONE confirmation if any work is in
            // flight (ARCHITECTURE.md "Session lifetime").
            const auto tabs = app->GetAllTabs();
            std::vector<std::string> ids;
            for (const auto& tab : tabs) ids.push_back(tab.session_id);
            if (!any_work_in_flight(tabs, ids)) {
                quit();
            } else {
                pending_close = PendingClose{};
                pending_close.quit = true;
                pending_close.session_ids = std::move(ids);
                show_quit_confirm = true;
            }
            return true;
        }
        return false;
    });

    // Modal layer, topmost first: the security UI (permission prompt) above
    // the quit confirmation above the frame. E4 has no plugin modals to
    // contend with; the never-preempted invariant is structural here, and E5
    // tests it against plugin modals.
    auto with_quit_confirm = Modal(with_keys, confirm_modal, &show_quit_confirm);
    auto with_permission = Modal(with_quit_confirm, permission_host_interactive, &show_permission);
    auto with_picker = Modal(with_permission, picker_host_interactive, &show_picker);
    Component tree = with_picker;

    // THE PUMP: reader threads wake nothing by themselves; this loop redraws
    // at 20 Hz from snapshots. Bounded join (one tick) at shutdown.
    std::atomic<bool> running{true};
    std::thread pump([&] {
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(tree);
    running.store(false, std::memory_order_release);
    pump.join();

    // Closing the TUI closes every tab: terminate all attached sessions
    // (ARCHITECTURE.md "Session lifetime" -- nothing outlives its client).
    app->CloseAllTabs();
    return kOk;
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
    return RunTui(parsed);
}
