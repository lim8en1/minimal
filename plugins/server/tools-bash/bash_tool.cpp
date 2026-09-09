#include "bash_tool.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>

#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "background_registry.hpp"
#include "bash_process.hpp"
#include "command_scanner.hpp"
#include "minimal/sidecar/drain.hpp"

namespace minimal::tools_bash {
namespace {

using tools::GrantOffer;
using tools::PermissionDisclosure;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

std::int64_t ElapsedMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

// --- Synchronous execution ---------------------------------------------------------
//
// The whole process lifecycle on the CALLING thread -- the session worker,
// which is the one thread permitted to block. The drain-and-reap loop itself
// is the SHARED sidecar runner (minimal/sidecar/drain.hpp) -- the same one
// the background watcher and the python plugin's captured runs use; what
// remains here is this tool's per-pipe policy (a capped output sink with a
// live emit tap, a bounded fd-9 state sink) and the state adoption rules.

struct RunOutcome {
    bool io_error = false;
    std::string io_error_message;
    bool timed_out = false;
    bool cancelled = false;
    // waitpid returned -1 (ECHILD): the pid was reaped out from under us, so
    // wait_status is meaningless and must not be read as "exit 0".
    bool reap_error = false;
    int wait_status = 0;   // valid unless the spawn itself failed
    std::string output;    // the capped, rendered stdout+stderr captured so far
    std::string state_payload;
    bool state_overflow = false;  // fd-9 payload exceeded the cap; state untouched
};

RunOutcome RunProcess(const LaunchedProcess& proc, const BashSettings& settings,
                      std::int64_t timeout_ms, ToolContext& ctx) {
    // DrainAndReap takes the process handles by value and closes them; the
    // LaunchedProcess struct carries raw ints, so they move into ScopedFds
    // here -- the same ownership transfer the local wrappers used to express.
    minimal::sidecar::Launched launched;
    launched.ok = true;
    launched.pid = proc.pid;
    launched.parent_fds.push_back(ScopedFd(proc.out_fd));
    launched.parent_fds.push_back(ScopedFd(proc.state_fd));

    std::vector<minimal::sidecar::PipeSink> sinks(2);
    sinks[0].mode = minimal::sidecar::PipeSink::Mode::kCapped;
    sinks[0].cap = settings.max_output_bytes;
    // Live channel to the operator, lossy by design; the capped record is
    // the sink's rendered text below.
    if (ctx.emit_output) sinks[0].emit = ctx.emit_output;
    sinks[1].mode = minimal::sidecar::PipeSink::Mode::kBounded;
    sinks[1].cap = kMaxStatePayloadBytes;

    minimal::sidecar::DrainOptions options;
    options.timeout_ms = timeout_ms;

    // Sliced inside the runner so cancellation is noticed within a slice,
    // not at the deadline.
    const auto drained = minimal::sidecar::DrainAndReap(
        std::move(launched), std::move(sinks), options,
        [&] { return ctx.is_cancelled && ctx.is_cancelled(); });

    RunOutcome outcome;
    outcome.io_error = drained.io_error;
    outcome.io_error_message = drained.io_error_message;
    outcome.timed_out = drained.timed_out;
    outcome.cancelled = drained.cancelled;
    outcome.reap_error = drained.reap_error;
    outcome.wait_status = drained.wait_status;
    outcome.output = std::move(drained.sinks[0].text);
    outcome.state_payload = std::move(drained.sinks[1].text);
    outcome.state_overflow = drained.sinks[1].overflow;
    return outcome;
}

// Resolves a command word against the SESSION's PATH, so `export PATH=...` in an
// earlier call changes what a later one names. Display and grant keys use the
// result: `cp /bin/bash /tmp/git` then `git ...` discloses /tmp/git, not `git`.
// That does not *prevent* anything -- nothing at this layer does -- it makes it
// visible at the moment consent is given, which is the only place it can be.
std::optional<std::filesystem::path> ResolveOnPath(const SessionShellState& state,
                                                    const std::string& word) {
    std::error_code ec;
    if (word.find('/') != std::string::npos) {
        std::filesystem::path p = state.cwd;
        p /= word;  // absolute `word` replaces, relative appends
        const auto canonical = std::filesystem::canonical(p, ec);
        // is_regular_file, same as the PATH branch below -- the two branches
        // had drifted, and without it `access(dir, X_OK)` succeeds for a
        // DIRECTORY, offering a grant for a path that cannot be a program.
        // Over-discloses only, but disclosure is what consent is given against.
        if (ec) return std::nullopt;
        const bool regular = std::filesystem::is_regular_file(canonical, ec);
        if (!regular || ec || access(canonical.c_str(), X_OK) != 0) return std::nullopt;
        return canonical;
    }

    const auto path_it = state.env.find("PATH");
    if (path_it == state.env.end()) return std::nullopt;

    std::string_view dirs = path_it->second;
    for (;;) {
        const auto colon = dirs.find(':');
        const std::string_view dir = dirs.substr(0, colon);
        // An empty element means the current directory, like in a shell.
        const auto candidate = dir.empty() ? state.cwd / word : std::filesystem::path(dir) / word;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec &&
            access(candidate.c_str(), X_OK) == 0) {
            // Canonical so the grant key is stable: /var/... and /private/var/...
            // are the same binary, and must not need granting twice.
            const auto canonical = std::filesystem::canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
        // The check precedes the break so the LAST element is searched too.
        if (colon == std::string_view::npos) break;
        dirs.remove_prefix(colon + 1);
    }
    return std::nullopt;
}

// --- The tool ------------------------------------------------------------------------

class BashTool : public tools::Tool {
public:
    BashTool(BashSettings settings, BashSessionStateStore& store,
             BackgroundTaskRegistry& background, hooks::HookPoint before_command)
        : settings_(std::move(settings)),
          store_(store),
          background_(background),
          before_command_(std::move(before_command)) {
        spec_.name = "bash";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Run a shell command. Prefer piping to a filter (`| tail -50`, `| grep -i error`) "
            "over returning a huge log, since everything returned enters the context. The "
            "working directory and environment PERSIST across calls: use `cd` and `export` "
            "normally, and pass restart=true to reset both. stdout and stderr are returned "
            "together; very long output is truncated in the middle; a non-zero exit status is "
            "an error. Set run_in_background=true for long-running commands.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}, {"description", "The shell command to run"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Wall-clock limit in seconds; defaults to " +
                                    std::to_string(settings_.timeout_ms / 1000) + ", capped at " +
                                    std::to_string(settings_.max_timeout_ms / 1000)}}},
              {"restart",
               {{"type", "boolean"},
                {"description", "Reset cwd and environment to their defaults before running"}}},
              {"run_in_background",
               {{"type", "boolean"},
                {"description",
                 "Run without waiting. Returns a task id and pid immediately; the result is "
                 "reported automatically at a turn boundary when the command finishes, and "
                 "the task is killed when the session ends. To read output mid-run, redirect "
                 "it to a file in the command itself. Kill the whole process group with: "
                 "kill -- -PID"}}}}},
            {"required", nlohmann::json::array({"command"})}};
    }

    const ToolSpec& spec() const override { return spec_; }

    // Not parallel-safe: the per-session cwd/env map below is adopted wholesale
    // after each command, so two concurrent invocations in one session would
    // race the adopt step (`cd A` in one call landing under another call's
    // result). Serialized per session by the agent loop; see Tool::parallel_safe.
    bool parallel_safe() const override { return false; }

    // Pattern rules match the raw command line (trimmed): the one subject bash
    // has. The executor matches with the command flavor -- '/' is an ordinary
    // character, so "git *" covers "git status src/x".
    std::optional<std::string> PatternSubject(const nlohmann::json& input) const override {
        if (!input.is_object() || !input.contains("command") || !input.at("command").is_string()) {
            return std::nullopt;
        }
        std::string command = input.at("command").get<std::string>();
        const auto first = command.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::nullopt;
        command.erase(0, first);
        const auto last = command.find_last_not_of(" \t\r\n");
        command.erase(last + 1);
        return command;
    }

    // Bash keeps the interface default for path confinement: there is no sound
    // way to know which paths an arbitrary command touches. A background
    // command is the same command string through the same shell, so disclosure
    // and the grant conjunction are identical either way -- run_in_background
    // changes WHEN the operator waits, never WHAT is being approved.
    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const ToolContext& ctx) const override {
        PermissionDisclosure disclosure;
        if (!input.contains("command") || !input.at("command").is_string()) {
            disclosure.summary = "bash: <invalid input>";
            disclosure.complete = false;
            return disclosure;
        }
        const auto command = input.at("command").get<std::string>();
        disclosure.summary = "bash: " + command;

        const auto state = store_.Snapshot(ctx.session_id, ctx.project_root);
        const auto scan = ScanCommand(command);
        disclosure.complete = scan.complete;

        for (const auto& word : scan.command_words) {
            const auto resolved = ResolveOnPath(state, word);
            // A word that resolves to nothing -- a builtin like `export`, or a
            // typo that will fail at exec -- has no binary to grant, so it
            // contributes no offer.
            //
            // No builtin table, deliberately. It would have to track the
            // *configured* shell's builtin set to be right, and getting it wrong
            // in the permissive direction is the dangerous half. Resolution
            // instead errs the safe way: macOS ships /usr/bin/cd, so `cd sub &&
            // ls` offers a grant for a binary the shell will never actually
            // exec. That over-discloses, which only ever makes the conjunction
            // stricter -- more keys to hold, never fewer.
            if (!resolved) continue;
            const auto path = resolved->string();
            disclosure.grants.push_back(GrantOffer{"bash:" + path, path + " *"});
        }
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        const auto started = std::chrono::steady_clock::now();

        if (!input.contains("command") || !input.at("command").is_string()) {
            return Reject(ctx, started, "missing required string field: command", "<invalid>");
        }
        const auto command = input.at("command").get<std::string>();

        // Checked rather than value(): a mistyped flag is a clear error, not an
        // exception that would unwind out of execute() with no one to catch it.
        bool restart = false;
        if (input.contains("restart")) {
            if (!input.at("restart").is_boolean()) {
                return Reject(ctx, started, "restart must be a boolean", command);
            }
            restart = input.at("restart").get<bool>();
        }

        bool run_in_background = false;
        if (input.contains("run_in_background")) {
            if (!input.at("run_in_background").is_boolean()) {
                return Reject(ctx, started, "run_in_background must be a boolean", command);
            }
            run_in_background = input.at("run_in_background").get<bool>();
        }

        std::int64_t timeout_ms = settings_.timeout_ms;
        bool timeout_supplied = false;
        if (input.contains("timeout")) {
            if (!input.at("timeout").is_number_integer()) {
                return Reject(ctx, started, "timeout must be an integer", command);
            }
            const auto seconds = input.at("timeout").get<std::int64_t>();
            if (seconds <= 0) {
                return Reject(ctx, started, "timeout must be positive", command);
            }
            timeout_ms = seconds * 1000;
            timeout_supplied = true;
        }

        // --- tools-bash.before_command: this plugin's own hook point ----------
        //
        // Veto-capable and synchronous, on the session worker: the natural
        // seam is right before spawn, so a guard script can refuse one
        // invocation at a finer grain than core's pre_tool_use -- the payload
        // carries the RESOLVED program path (the same disclosure-time
        // resolution the Ask prompt shows, session PATH included), not just
        // the tool name. A block becomes an error ToolResult; a broken script
        // fails open at the dispatcher, never here.
        //
        // Resolved against a pre-reset snapshot, and placed BEFORE the
        // restart below: a blocked invocation must not have wiped the
        // session's cwd and environment on its way out -- the same rule the
        // restart line's own comment states.
        if (before_command_.enabled()) {
            nlohmann::json payload{{"session_id", ctx.session_id}, {"command", command}};
            const auto hook_state = store_.Snapshot(ctx.session_id, ctx.project_root);
            const auto scan = ScanCommand(command);
            for (const auto& word : scan.command_words) {
                if (auto program = ResolveOnPath(hook_state, word)) {
                    payload["program"] = program->string();
                    break;
                }
            }
            if (auto blocked = before_command_.Emit(std::move(payload), ctx.is_cancelled)) {
                ctx.logger.warn("bash[{}] before_command hook blocked '{}': {}", ctx.session_id,
                                command, *blocked);
                return Error("blocked by hook: " + *blocked);
            }
        }

        // After validation, never before: a rejected invocation must not have
        // wiped the session's cwd and environment on its way out.
        if (restart) store_.Reset(ctx.session_id, ctx.project_root);

        if (run_in_background) return ExecuteBackground(ctx, command, timeout_ms, timeout_supplied);

        timeout_ms = std::min(timeout_ms, settings_.max_timeout_ms);
        const auto state = store_.Snapshot(ctx.session_id, ctx.project_root);

        const auto proc =
            LaunchProcess(WrapCommand(command), state, settings_, timeout_ms);
        if (!proc.ok) {
            const auto duration_ms = ElapsedMs(started);
            ctx.logger.warn("bash[{}] spawn failed: {} ({} ms)", ctx.session_id, proc.spawn_error,
                            duration_ms);
            return Error("failed to start shell: " + proc.spawn_error);
        }

        const auto outcome = RunProcess(proc, settings_, timeout_ms, ctx);
        const auto duration_ms = ElapsedMs(started);

        if (outcome.io_error) {
            ctx.logger.warn("bash[{}] io error running '{}': {} ({} ms)", ctx.session_id, command,
                            outcome.io_error_message, duration_ms);
            return Error("failed reading command output: " + outcome.io_error_message);
        }

        // The captured output leads every failure result: it is usually the most
        // useful thing the model can see (the error message, the partial log).
        const auto with_output = [&](const std::string& footer) {
            return outcome.output.empty() ? footer : outcome.output + "\n" + footer;
        };

        if (outcome.cancelled) {
            ctx.logger.info("bash[{}] cancelled '{}' after {} ms", ctx.session_id, command,
                            duration_ms);
            return Error(with_output("[cancelled after " + std::to_string(duration_ms) + " ms]"));
        }
        if (outcome.timed_out) {
            ctx.logger.warn("bash[{}] timed out: '{}' ({} ms)", ctx.session_id, command,
                            duration_ms);
            return Error(with_output("[timed out after " + std::to_string(timeout_ms) +
                                     " ms; process group killed]"));
        }

        // Adoption happens only AFTER the failure returns above, and that
        // ordering is load-bearing: a cancelled or timed-out command was
        // killed mid-drain, so a payload that still parses is a truncated
        // prefix of the real trailer -- adopting it wholesale-replaces the
        // session env with a partial one (a dropped PATH strands every later
        // call). Same all-or-nothing rule as the overflow latch in
        // RunProcess; the all-or-nothing PARSE (ParseStatePayload) cannot see
        // this case, because a valid prefix is indistinguishable from a
        // complete state.
        if (outcome.state_overflow) {
            ctx.logger.warn("bash[{}] fd-9 state payload exceeded {} bytes; cwd and environment "
                            "left unchanged",
                            ctx.session_id, kMaxStatePayloadBytes);
        } else {
            AdoptShellState(ctx, outcome.state_payload);
        }

        if (outcome.reap_error) {
            // ECHILD: the pid was reaped out from under us, so the status in
            // wait_status is meaningless. Reporting it (0 would mean success)
            // instead of silently calling the command a clean exit.
            return Error(with_output(
                "[could not determine exit status (process already reaped)]"));
        }
        if (WIFSIGNALED(outcome.wait_status)) {
            const int sig = WTERMSIG(outcome.wait_status);
            ctx.logger.warn("bash[{}] '{}' terminated by signal {} ({} ms)", ctx.session_id,
                            command, sig, duration_ms);
            return Error(with_output("[terminated by signal " + std::to_string(sig) + "]"));
        }

        const int code = WIFEXITED(outcome.wait_status) ? WEXITSTATUS(outcome.wait_status) : -1;
        if (code == 0) {
            ctx.logger.info("bash[{}] ok: '{}' ({} ms)", ctx.session_id, command, duration_ms);
            return ToolResult{false, outcome.output};
        }
        ctx.logger.warn("bash[{}] exit {}: '{}' ({} ms)", ctx.session_id, code, command,
                        duration_ms);
        return Error(with_output("[exit status: " + std::to_string(code) + "]"));
    }

private:
    // The background path. The tool call returns an acknowledgment immediately;
    // the watcher thread drains output, reaps, and reports the result through
    // the deferred-completion handles on the context. Session state is NOT
    // adopted from a background command: its final cwd/env is stale by the time
    // it reports, and adopting it would stomp whatever foreground calls set up
    // in between.
    ToolResult ExecuteBackground(ToolContext& ctx, const std::string& command,
                                 std::int64_t timeout_ms, bool timeout_supplied) {
        if (!ctx.schedule_injection) {
            // Without the injection channel the result would be silently lost
            // at completion. The synchronous path always works; refuse this
            // one with the reason rather than start work nobody can collect.
            return Error("background execution is unavailable in this session "
                         "(no deferred-completion channel)");
        }
        // A background task's budget is its own ceiling: the sync default (2
        // minutes) exists to bound a BLOCKED WORKER, which a background task
        // never holds. When the model supplies no timeout, the ceiling IS the
        // budget -- a long-running build is the entire point of the mode.
        const std::int64_t bg_timeout =
            std::min(timeout_supplied ? timeout_ms : settings_.background_max_timeout_ms,
                     settings_.background_max_timeout_ms);

        const auto state = store_.Snapshot(ctx.session_id, ctx.project_root);

        BackgroundTaskRegistry::WatchHandles handles;
        handles.emit_output = ctx.emit_output;
        handles.schedule_injection = ctx.schedule_injection;
        handles.emit_task_event = ctx.emit_task_event;
        handles.session_lifetime = ctx.session_lifetime;
        handles.tool_use_id = ctx.tool_use_id;
        handles.logger = &ctx.logger;

        std::string error;
        auto started_task =
            background_.Start(ctx.session_id, command, state, settings_, bg_timeout,
                              std::move(handles), &error);
        if (!started_task) {
            ctx.logger.warn("bash[{}] background start rejected: {} ({})", ctx.session_id, error,
                            command);
            return Error(error);
        }
        ctx.logger.info("bash[{}] background {}: '{}' (pid {}, timeout {} ms)", ctx.session_id,
                        started_task->id, command, started_task->pid, bg_timeout);

        // The acknowledgment. pid == pgid (the child setsid()s), so `kill -- -pid`
        // reaches the whole tree -- the model can clean up its own task with an
        // ordinary bash call. The polling hint steers toward the redirect-to-file
        // pattern instead of a bespoke output tool: pipes and redirection are the
        // reason this tool has no schema knobs, and background changes nothing.
        return ToolResult{
            false,
            "running in background as task " + started_task->id + " (pid " +
                std::to_string(started_task->pid) + ", timeout " +
                std::to_string(bg_timeout / 1000) + " s). The result will be reported "
                "automatically at a turn boundary once it finishes; this session ends, so does "
                "the task. To follow progress mid-run, redirect output to a file and read it "
                "with later calls; to stop the task: kill -- -" +
                std::to_string(started_task->pid)};
    }

    void AdoptShellState(ToolContext& ctx, const std::string& payload) {
        if (payload.empty()) return;  // the command exited before the trailer ran
        const ShellState reported = ParseStatePayload(payload);
        if (!reported.ok) {
            ctx.logger.warn("bash[{}] ignoring malformed shell state ({} bytes); "
                            "cwd and environment left unchanged",
                            ctx.session_id, payload.size());
            return;
        }
        store_.SetState(ctx.session_id, reported.cwd, reported.env);
    }

    ToolResult Reject(ToolContext& ctx, std::chrono::steady_clock::time_point started,
                      const std::string& reason, const std::string& command) {
        ctx.logger.info("bash[{}] rejected: {} (command: '{}', {} ms)", ctx.session_id, reason,
                        command, ElapsedMs(started));
        return Error(reason);
    }

    ToolSpec spec_;
    BashSettings settings_;
    BashSessionStateStore& store_;
    BackgroundTaskRegistry& background_;
    // This plugin's veto-capable hook point. See plugin.cpp for registration
    // and kBeforeCommandPoint for the name.
    hooks::HookPoint before_command_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeBashTool(BashSettings settings, BashSessionStateStore& store,
                                          BackgroundTaskRegistry& background,
                                          hooks::HookPoint before_command) {
    return std::make_unique<BashTool>(std::move(settings), store, background,
                                      std::move(before_command));
}

}  // namespace minimal::tools_bash
