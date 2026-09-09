#include "python_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>

#include "minimal/common/logger.hpp"

#include "driver_source.hpp"
#include "python_env.hpp"

namespace minimal::tools_python {
namespace {

using tools::BackgroundTaskEvent;
using tools::PermissionDisclosure;
using tools::ToolResult;

constexpr std::size_t kMaxCodeBytes = 1024 * 1024;

std::string FirstLine(const std::string& text, std::size_t max) {
    auto end = text.find('\n');
    std::string line = text.substr(0, end == std::string::npos ? text.size() : end);
    if (line.size() > max) line = line.substr(0, max) + "...";
    return line;
}

RequestHandles BuildHandles(const std::string& tool_use_id, const std::string& code,
                            tools::ToolContext& ctx) {
    RequestHandles handles;
    handles.tool_use_id = tool_use_id;
    handles.label = FirstLine(code, 80);
    handles.schedule_injection = ctx.schedule_injection;
    handles.emit_task_event = ctx.emit_task_event;
    handles.emit_output = ctx.emit_output;
    handles.session_lifetime = ctx.session_lifetime;
    return handles;
}

void EmitStarted(const RequestHandles& handles, const std::string& id, pid_t pid) {
    if (!handles.emit_task_event) return;
    try {
        BackgroundTaskEvent event;
        event.task_id = id;
        event.tool_use_id = handles.tool_use_id;
        event.command = handles.label;
        event.pid = pid;
        event.state = BackgroundTaskEvent::State::kStarted;
        handles.emit_task_event(event);
    } catch (...) {
    }
}

}  // namespace

// --- python_exec -----------------------------------------------------------------

namespace {

class PythonExecTool : public tools::Tool {
public:
    PythonExecTool(PythonSettings settings, InterpreterStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "python_exec";
        spec_.category = tools::ToolCategory::Execute;
        spec_.definition.name = "python_exec";
        spec_.definition.description =
            "Run code in this session's persistent Python interpreter: state (globals, "
            "imports, live connections) survives across calls and turns. Companions: "
            "python_output (poll background work), python_interrupt (stop one), "
            "python_input (feed stdin). Environment is shared per-project: "
            "<project_root>/.venv, created on demand when the agent preset declares "
            "packages. All times are SECONDS.";
        spec_.definition.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"code",
               {{"type", "string"},
                {"description", "Python source to execute in the persistent interpreter. A "
                                "trailing expression's value is returned as the result."}}},
              {"timeout",
               {{"type", "integer"},
                {"description",
                 "Seconds to wait for a result. 0 (default) = async: returns 'py-N started' "
                 "immediately; the result arrives as a tool result before your next reply. "
                 ">0 = wait synchronously up to N seconds; if the code is still running at "
                 "the deadline, the wait converts to async -- a sync wait never kills "
                 "anything."}}},
              {"max_runtime",
               {{"type", "integer"},
                {"description",
                 "Deadline for the request itself, counted from when it starts running "
                 "(queue time excluded; 0 or omitted = unlimited). On expiry the request is "
                 "interrupted; if it ignores the interrupt, the whole interpreter is "
                 "hard-killed and its state is LOST -- the only timer that destroys "
                 "anything."}}},
              {"clean",
               {{"type", "boolean"},
                {"description", "Restart the interpreter before running: all previous state "
                                "is discarded (your explicit choice -- the recovery for a "
                                "wedged interpreter, e.g. a stuck non-interruptible C "
                                "call)."}}}}},
            {"required", {"code"}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "python"; }
    bool parallel_safe() const override { return false; }

    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const tools::ToolContext& ctx) const override {
        PermissionDisclosure disclosure;
        disclosure.summary = "python_exec: " + FirstLine(input.value("code", std::string()), 100);
        // The interpreter the invocation would run in, resolved without I/O
        // side effects beyond an exists() probe -- the same lower-bound rule
        // as bash's disclosure: it names what will run.
        const auto slice = ParseSlice(ctx.tool_config);
        const auto plan = MakeEnvPlan(ctx.project_root, slice, settings_);
        const auto binary = ResolveInterpreterBinary(plan);
        disclosure.grants.push_back({"python_exec:" + binary, binary + " *"});
        disclosure.complete = true;
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const auto code_it = input.find("code");
        if (code_it == input.end() || !code_it->is_string() || code_it->get<std::string>().empty()) {
            return Error("python_exec requires a non-empty 'code' string");
        }
        const auto code = code_it->get<std::string>();
        if (code.size() > kMaxCodeBytes) {
            return Error("code exceeds " + std::to_string(kMaxCodeBytes) + " bytes; write it to a "
                         "file with the write tool and exec the file instead");
        }
        std::int64_t timeout = 0;
        if (auto it = input.find("timeout"); it != input.end() && it->is_number_integer() &&
                                                it->get<std::int64_t>() >= 0) {
            timeout = it->get<std::int64_t>();
        }
        // The request's own kill deadline: absent -> the operator's setting
        // (default unlimited); explicit 0 = unlimited; negative is ignored.
        std::int64_t max_runtime = settings_.background_max_runtime;
        if (auto it = input.find("max_runtime"); it != input.end() && it->is_number_integer() &&
                                                       it->get<std::int64_t>() >= 0) {
            max_runtime = it->get<std::int64_t>();
        }
        const bool clean = input.value("clean", false);

        const auto slice = ParseSlice(ctx.tool_config);
        const auto plan = MakeEnvPlan(ctx.project_root, slice, settings_);

        if (clean) store_.Erase(ctx.session_id, "clean restart requested");

        auto interp = store_.Find(ctx.session_id);
        std::string advisory;
        if (!interp || !interp->alive()) {
            // (Re)start: materialize the environment, then spawn.
            std::string ensure_error;
            std::string note;
            if (plan.wants_venv) {
                note = EnsureEnvironment(plan, settings_, ctx.logger, &ensure_error);
                if (!ensure_error.empty()) {
                    return Error("could not prepare the python environment: " + ensure_error +
                                 "\nThe interpreter was not started; nothing was lost.");
                }
            }
            InterpreterConfig config;
            config.python_path = plan.wants_venv ? plan.venv_python
                                                 : ResolveInterpreterBinary(plan);
            config.cwd = ctx.project_root;
            config.venv = plan.wants_venv ? plan.venv : std::filesystem::path{};
            config.declared = plan.declared;
            config.env = BuildChildEnv(plan, settings_);
            config.session_lifetime = ctx.session_lifetime;
            std::string spawn_error;
            interp = Interpreter::Spawn(config, settings_, ctx.logger, &spawn_error);
            if (!interp) {
                return Error("failed to start the python interpreter: " + spawn_error);
            }
            store_.Add(ctx.session_id, interp);
            advisory = "[started " + interp->version() + " in " + config.python_path +
                       (note.empty() ? "]" : "; " + note + "]");
        } else {
            const auto diff_error = ApplyDiff(interp, plan, ctx, &advisory);
            if (!diff_error.empty()) return Error(diff_error);
        }

        RequestHandles handles = BuildHandles(ctx.tool_use_id, code, ctx);
        const bool async = timeout == 0;
        std::string submit_error;
        const auto id = interp->Submit(code, async, std::move(handles), &submit_error, max_runtime);
        if (!id) {
            return Error(submit_error +
                         "\nThe interpreter is gone; the next python_exec starts a fresh one.");
        }

        if (async) {
            EmitStarted(BuildHandles(ctx.tool_use_id, code, ctx), *id, interp->pid());
            std::string ack =
                *id + " started; its result arrives as a tool result before your next reply. "
                      "python_output polls progress; python_interrupt stops it.";
            if (max_runtime > 0) ack += " Deadline: " + std::to_string(max_runtime) + "s.";
            return Ok(ack + advisory);
        }

        std::int64_t wait = timeout;
        if (settings_.sync_wait_ceiling > 0 && wait > settings_.sync_wait_ceiling) {
            wait = settings_.sync_wait_ceiling;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
        RequestOutcome outcome;
        switch (interp->WaitSync(*id, ctx.cancel_token, deadline, &outcome)) {
            case Interpreter::WaitOutcome::kCompleted: {
                const auto queued = interp->PendingIds().size();
                ToolResult result;
                result.is_error = !outcome.ok;
                result.content_text =
                    FormatOutcome(*id, outcome, queued, settings_.max_output_bytes) + advisory;
                return result;
            }
            case Interpreter::WaitOutcome::kDemoted:
                return Ok(*id + " is still running after " + std::to_string(wait) +
                          "s; its result arrives as a tool result before your next reply. "
                          "python_output polls progress; python_interrupt stops it." +
                          advisory);
            case Interpreter::WaitOutcome::kCancelled:
                return Ok(*id + " was cancelled while waiting; it continues in the interpreter "
                                "and its result arrives as a tool result before your next "
                                "reply. python_output polls progress." +
                          advisory);
            case Interpreter::WaitOutcome::kDead:
            default:
                return Error(*id + " was lost: the interpreter died before replying. The next "
                                   "python_exec starts a fresh one.");
        }
    }

private:
    static ToolResult Ok(std::string text) { return ToolResult{false, std::move(text)}; }
    static ToolResult Error(std::string text) { return ToolResult{true, std::move(text)}; }

    // The per-exec comparison (the preset switch machinery). Empty error
    // string = proceed; `advisory` gains the result-text notes.
    std::string ApplyDiff(const std::shared_ptr<Interpreter>& interp, const EnvPlan& plan,
                          tools::ToolContext& ctx, std::string* advisory) {
        const auto diff = ClassifyDiff(plan.declared, interp->Declared());
        std::string text;
        if (!interp->config().venv.empty()) {
            if (!diff.additive.empty()) {
                std::string install_error;
                const auto note = EnsurePackages(ctx.project_root, plan.venv_python,
                                                 diff.additive, settings_, ctx.logger,
                                                 &install_error);
                if (!install_error.empty()) {
                    return "installing the preset's newly declared packages failed: " +
                           install_error + "\nThe interpreter and its state are untouched; "
                                           "install manually or adjust the preset.";
                }
                interp->AdoptDeclared(diff.additive);
                text += note;
            }
            if (!diff.conflicts.empty() && interp->NoticeShown("declared_conflict")) {
                std::string joined;
                for (std::size_t i = 0; i < diff.conflicts.size(); ++i) {
                    if (i != 0) joined += ", ";
                    joined += diff.conflicts[i];
                }
                text += (text.empty() ? "" : " ") + std::string("[declared conflict: ") + joined +
                        " -- the preset now pins a different version than this interpreter runs; "
                        "upgrading would affect every agent in this project. Install manually, "
                        "or python_exec(clean=true) to restart in the new environment]";
            }
        } else if (plan.wants_venv) {
            // Bare interpreter, but the environment now wants the venv (the
            // preset declares packages, or .venv appeared). Materialize it for
            // the restart; the RUNNING interpreter cannot use it.
            std::string ensure_error;
            EnsureEnvironment(plan, settings_, ctx.logger, &ensure_error);
            if (!ensure_error.empty()) {
                ctx.logger.warn("python[{}]: venv materialization failed: {}", ctx.session_id,
                                ensure_error);
            }
            if (interp->NoticeShown("bare_drift")) {
                text += "[this interpreter runs system python; the project venv now exists -- "
                        "python_exec(clean=true) to move into it]";
            }
        }
        *advisory += text.empty() ? "" : " " + text;
        return {};
    }

    PythonSettings settings_;
    InterpreterStore& store_;
    tools::ToolSpec spec_;
};

}  // namespace

// --- python_interrupt --------------------------------------------------------------

namespace {

class PythonInterruptTool : public tools::Tool {
public:
    PythonInterruptTool(PythonSettings settings, InterpreterStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "python_interrupt";
        spec_.category = tools::ToolCategory::Execute;
        spec_.definition.name = "python_interrupt";
        spec_.definition.description =
            "Stop work in this session's Python interpreter.\n"
            "kill = false (default): send SIGINT -- the currently-running request fails with "
            "KeyboardInterrupt and the interpreter SURVIVES with all its state. Queued requests "
            "then run. Reports whether the interrupt landed (a non-interruptible C extension "
            "may not react).\n"
            "kill = true: SIGKILL the whole interpreter. All state is lost (explicit choice); "
            "pending requests fail; the next python_exec starts fresh. This is the only stop "
            "for a request stuck inside a non-interruptible C call.";
        spec_.definition.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"kill",
               {{"type", "boolean"},
                {"description",
                 "false (default): interrupt the running request, keep the interpreter and its "
                 "state. true: kill the entire interpreter; all state is lost."}}}}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "python"; }
    bool parallel_safe() const override { return false; }

    ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const bool kill = input.value("kill", false);
        auto interp = store_.Find(ctx.session_id);
        if (!interp || !interp->alive()) {
            return ToolResult{true, "no python interpreter is running in this session"};
        }
        if (kill) {
            store_.Erase(ctx.session_id, "killed by python_interrupt(kill=true)");
            return ToolResult{
                false, "interpreter killed; all state discarded. The next python_exec starts a "
                       "fresh interpreter in the current environment."};
        }
        interp->SoftInterrupt();
        switch (interp->WaitForInterrupt(std::chrono::seconds(settings_.grace_seconds),
                                         ctx.cancel_token)) {
            case Interpreter::InterruptOutcome::kResponse:
                return ToolResult{false,
                                  "interrupt delivered: the running request failed with "
                                  "KeyboardInterrupt; the interpreter and its state survive. "
                                  "Queued requests continue."};
            case Interpreter::InterruptOutcome::kIdleInterrupt:
                return ToolResult{false, "nothing was running; the interpreter is idle and "
                                         "unaffected"};
            case Interpreter::InterruptOutcome::kNoReaction:
            default:
                return ToolResult{
                    true, "SIGINT sent but no reaction within " +
                              std::to_string(settings_.grace_seconds) +
                              "s -- likely a non-interruptible C extension. Use kill=true to "
                              "stop it (all state is lost)."};
        }
    }

private:
    PythonSettings settings_;
    InterpreterStore& store_;
    tools::ToolSpec spec_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakePythonExecTool(PythonSettings settings, InterpreterStore& store) {
    return std::make_unique<PythonExecTool>(std::move(settings), store);
}

// --- python_output -----------------------------------------------------------------

namespace {

class PythonOutputTool : public tools::Tool {
public:
    PythonOutputTool(PythonSettings settings, InterpreterStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "python_output";
        spec_.category = tools::ToolCategory::Execute;
        spec_.definition.name = "python_output";
        spec_.definition.description =
            "Poll background work in this session's Python interpreter without blocking.\n"
            "No arguments (or scope='all'): one status line per task -- running (age, live "
            "output size), queued behind N, or finished (ok/failed, runtime; its result "
            "arrives with the next turn if not already delivered).\n"
            "task_id + scope: that task's output. scope is 'top:N', 'tail:N', or 'read:N:M' "
            " -- byte-offset slices of the task's output stream (a running task's live ring; "
            "a finished task's final capture). Responses carry the stream total and "
            "next_offset for paging; output that fell out of a running task's ring comes "
            "back as an explicit '[... N bytes lost ...]' marker.\n"
            "grep (optional regex): filters the sliced text line-by-line; matches report "
            "their byte offsets within the slice.";
        spec_.definition.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"task_id",
               {{"type", "string"},
                {"description", "A py-N id from python_exec's acknowledgment. Omit to list "
                                "all tasks."}}},
              {"scope",
               {{"type", "string"},
                {"description", "'all' (list tasks), or with task_id: 'top:N', 'tail:N' "
                                "(default: 8192), 'read:N,M' (byte offset N, length M)."}}},
              {"grep",
               {{"type", "string"},
                {"description", "Regex applied to the sliced output's lines after scoping; "
                                "matches report byte offsets within the slice."}}}}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "python"; }
    bool parallel_safe() const override { return false; }

    ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const nlohmann::json args =
            input.is_object() ? input : nlohmann::json::object();
        auto interp = store_.Find(ctx.session_id);
        if (!interp || !interp->alive()) {
            return Error("no python interpreter is running in this session");
        }
        const auto task_id = args.value("task_id", std::string());
        const auto scope = args.value("scope", std::string());
        const auto grep = args.value("grep", std::string());

        // Listing: every task, one line each (plus the running task's tail --
        // the thing a poll is usually for).
        if (task_id.empty() && (scope.empty() || scope == "all")) {
            const auto tasks = interp->TaskStatuses();
            if (tasks.empty()) {
                return Ok("no python tasks in this session");
            }
            std::string text;
            for (const auto& t : tasks) {
                if (!text.empty()) text += "\n";
                switch (t.state) {
                    case TaskStatus::State::kRunning:
                        text += t.id + " -- running " + Seconds(t.seconds) + ", " +
                                Bytes(t.stream_total) + " output so far";
                        if (t.max_runtime > 0) text += ", deadline " + std::to_string(t.max_runtime) + "s";
                        break;
                    case TaskStatus::State::kQueued:
                        text += t.id + " -- queued behind " + std::to_string(t.queued_behind) +
                                " request(s)";
                        break;
                    case TaskStatus::State::kFinished:
                        text += t.id + " -- finished (" + (t.ok ? "ok" : "failed") + ") in " +
                                Seconds(t.seconds) +
                                "; its result arrives with the next turn if not already "
                                "delivered";
                        break;
                }
            }
            // The running task's tail: live progress without a second call.
            for (const auto& t : tasks) {
                if (t.state != TaskStatus::State::kRunning) continue;
                const auto tail = interp->ReadTaskOutput(
                    t.id, t.stream_total > kListTailBytes ? t.stream_total - kListTailBytes : 0,
                    kListTailBytes);
                if (!tail.slice.text.empty()) {
                    text += "\n--- " + t.id + " output (tail) ---\n" + tail.slice.text;
                }
                break;
            }
            return Ok(text);
        }

        if (task_id.empty()) {
            return Error("scope '" + scope + "' needs a task_id (or omit scope to list tasks)");
        }

        // One task's output, sliced by absolute byte offset.
        std::uint64_t offset = 0;
        std::size_t length = kDefaultTailBytes;
        std::string scope_note = "tail 8192";
        if (scope.empty() || scope.rfind("tail:", 0) == 0) {
            const auto total = interp->ReadTaskOutput(task_id, 0, 0).slice.total;
            const auto n = ParseCount(scope, kDefaultTailBytes);
            const auto begin = total > n ? total - n : 0;
            offset = begin;
            length = static_cast<std::size_t>(total - begin);
            scope_note = "tail " + std::to_string(n);
        } else if (scope.rfind("top:", 0) == 0) {
            length = static_cast<std::size_t>(ParseCount(scope, kDefaultTailBytes));
            scope_note = "top " + std::to_string(length);
        } else if (scope.rfind("read:", 0) == 0) {
            const auto colon = scope.find(':');
            const auto comma = scope.find(',', colon + 1);
            if (comma == std::string::npos) {
                return Error("scope 'read:N:M' takes 'read:<offset>,<length>'");
            }
            offset = ParseCount(scope.substr(colon + 1, comma - colon - 1), 0);
            length = static_cast<std::size_t>(ParseCount(scope.substr(comma + 1), 0));
            scope_note = "read " + std::to_string(offset) + "+" + std::to_string(length);
        } else {
            return Error("unknown scope '" + scope +
                         "' (expected 'all', 'top:N', 'tail:N', or 'read:N,M')");
        }

        const auto read = interp->ReadTaskOutput(task_id, offset, length);
        if (!read.found) {
            return Error("no such task: " + task_id +
                         " (it may be older than the last few completions; python_output with "
                         "no arguments lists what is tracked)");
        }

        std::string text;
        // The task's state line comes free from the listing scan.
        for (const auto& t : interp->TaskStatuses()) {
            if (t.id == task_id) {
                switch (t.state) {
                    case TaskStatus::State::kRunning:
                        text = task_id + " -- running " + Seconds(t.seconds);
                        break;
                    case TaskStatus::State::kQueued:
                        text = task_id + " -- queued behind " + std::to_string(t.queued_behind) +
                               " request(s); no output yet";
                        break;
                    case TaskStatus::State::kFinished:
                        text = task_id + " -- finished (" + (t.ok ? "ok" : "failed") + ") in " +
                               Seconds(t.seconds);
                        break;
                }
                break;
            }
        }
        if (text.empty()) text = task_id;
        text += "\n[scope " + scope_note + "; stream " + std::to_string(read.slice.total) +
                " bytes; showing " + std::to_string(offset) + ".." +
                std::to_string(read.slice.next) + (read.finished ? " of the final capture" : "") +
                "]";
        if (read.slice.lost > 0) {
            text += "\n[" + std::to_string(read.slice.lost) +
                    " bytes inside this range are no longer retained]";
        }

        std::string body = read.slice.text;
        if (!grep.empty()) {
            std::string filtered;
            try {
                const std::regex pattern(grep);
                std::size_t pos = 0;
                while (pos < body.size()) {
                    const auto nl = body.find('\n', pos);
                    const auto line_end = nl == std::string::npos ? body.size() : nl + 1;
                    const std::string_view line(body.data() + pos, line_end - pos);
                    if (std::regex_search(line.begin(), line.end(), pattern)) {
                        filtered += "[" + std::to_string(pos) + "] " + std::string(line);
                        if (!filtered.empty() && filtered.back() != '\n') filtered += "\n";
                    }
                    pos = line_end;
                }
            } catch (const std::regex_error& e) {
                return Error("bad grep regex: " + std::string(e.what()));
            }
            body = filtered.empty() ? "(no matches)" : filtered;
        }
        if (body.empty()) body = "(no output in this range)";
        text += "\n" + body;
        if (read.slice.next < read.slice.total) {
            text += "\n[next: read:" + std::to_string(read.slice.next) + "," +
                    std::to_string(std::min<std::size_t>(kDefaultTailBytes, 65536)) + "]";
        }
        return Ok(text);
    }

private:
    static ToolResult Ok(std::string text) { return ToolResult{false, std::move(text)}; }
    static ToolResult Error(std::string text) { return ToolResult{true, std::move(text)}; }
    static std::string Seconds(double s) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << s << "s";
        return out.str();
    }
    static std::string Bytes(std::uint64_t n) {
        std::ostringstream out;
        if (n >= 1024 * 1024) {
            out << std::fixed << std::setprecision(1) << (n / (1024.0 * 1024.0)) << " MiB";
        } else if (n >= 1024) {
            out << std::fixed << std::setprecision(1) << (n / 1024.0) << " KiB";
        } else {
            out << n << " B";
        }
        return out.str();
    }
    static std::uint64_t ParseCount(const std::string& text, std::uint64_t fallback) {
        auto first = text.find_first_of("0123456789");
        if (first == std::string::npos) return fallback;
        auto last = text.find_first_not_of("0123456789", first);
        try {
            return static_cast<std::uint64_t>(std::stoull(text.substr(first, last)));
        } catch (...) {
            return fallback;
        }
    }

    static constexpr std::size_t kDefaultTailBytes = 8192;
    static constexpr std::size_t kListTailBytes = 2048;
    PythonSettings settings_;
    InterpreterStore& store_;
    tools::ToolSpec spec_;
};

}  // namespace

// --- python_input ------------------------------------------------------------------

namespace {

class PythonInputTool : public tools::Tool {
public:
    PythonInputTool(PythonSettings settings, InterpreterStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "python_input";
        spec_.category = tools::ToolCategory::Execute;
        spec_.definition.name = "python_input";
        spec_.definition.description =
            "Feed stdin of the code running in this session's Python interpreter. A blocking "
            "input() (or sys.stdin read) parks the interpreter until you call this; a newline "
            "is appended when the text lacks one. The write is non-blocking: a full pipe (the "
            "code is not consuming) is reported, never parked on.";
        spec_.definition.input_schema = nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"text",
               {{"type", "string"},
                {"description", "The stdin payload (a line is appended when absent)."}}}}},
            {"required", {"text"}}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "python"; }
    bool parallel_safe() const override { return false; }

    ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const auto it = input.find("text");
        if (it == input.end() || !it->is_string()) {
            return Error("python_input requires a 'text' string");
        }
        auto text = it->get<std::string>();
        if (text.empty() || text.back() != '\n') text += "\n";

        auto interp = store_.Find(ctx.session_id);
        if (!interp || !interp->alive()) {
            return Error("no python interpreter is running in this session");
        }
        std::string error;
        const auto accepted = interp->WriteStdin(text, &error);
        if (accepted == 0 && !error.empty()) {
            return Error(error);
        }
        if (!error.empty()) {
            return Error(std::to_string(accepted) + " of " + std::to_string(text.size()) +
                         " bytes delivered; " + error);
        }
        return Ok(std::to_string(accepted) + " bytes delivered to the interpreter's stdin");
    }

private:
    static ToolResult Ok(std::string text) { return ToolResult{false, std::move(text)}; }
    static ToolResult Error(std::string text) { return ToolResult{true, std::move(text)}; }
    PythonSettings settings_;
    InterpreterStore& store_;
    tools::ToolSpec spec_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakePythonOutputTool(PythonSettings settings,
                                                  InterpreterStore& store) {
    return std::make_unique<PythonOutputTool>(std::move(settings), store);
}

std::unique_ptr<tools::Tool> MakePythonInputTool(PythonSettings settings,
                                                 InterpreterStore& store) {
    return std::make_unique<PythonInputTool>(std::move(settings), store);
}

std::unique_ptr<tools::Tool> MakePythonInterruptTool(PythonSettings settings,
                                                     InterpreterStore& store) {
    return std::make_unique<PythonInterruptTool>(std::move(settings), store);
}

}  // namespace minimal::tools_python
