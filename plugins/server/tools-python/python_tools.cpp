#include "python_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include <re2/re2.h>

#include "minimal/common/logger.hpp"

#include "json_fields.hpp"
#include "python_env.hpp"

namespace minimal::tools_python {
namespace {

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

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ", ";
        out += items[i];
    }
    return out;
}

// The one-time advisory for declared entries that were NOT installed because
// the shared venv already carries a different spec for the same name.
std::string ConflictNotice(const std::vector<std::string>& conflicts) {
    return "[declared conflict: " + Join(conflicts) +
           " -- not installed: the project venv is shared, and re-versioning it would change "
           "the environment under code that already imported the other version. Install "
           "manually, or python_exec(clean=true) once no other session depends on it]";
}

}  // namespace

// --- python_exec -----------------------------------------------------------------

namespace {

class PythonExecTool : public tools::Tool {
public:
    PythonExecTool(std::shared_ptr<const common::LiveValue<PythonSettings>> live, InterpreterStore& store)
        : live_(std::move(live)), store_(store) {
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

    std::string Summary(const nlohmann::json& input) const override {
        return ">>> " + FirstLine(StringField(input, "code"), 200);
    }

    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const tools::ToolContext& ctx) const override {
        PermissionDisclosure disclosure;
        disclosure.summary = "python_exec: " + FirstLine(StringField(input, "code"), 100);
        // The interpreter the invocation would run in, resolved without I/O
        // side effects beyond an exists() probe -- the same lower-bound rule
        // as bash's disclosure: it names what will run.
        // A live interpreter runs its own binary; otherwise the one a spawn
        // would start (the venv's python whenever the plan wants the venv).
        std::string binary;
        if (const auto interp = store_.Find(ctx.session_id); interp && interp->alive()) {
            binary = interp->config().python_path;
        } else {
            const auto slice = ParseSlice(ctx.tool_config);
            binary = ResolveInterpreterBinary(MakeEnvPlan(ctx.project_root, slice, S()));
        }
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
        // Both feed `now() + std::chrono::seconds(N)`: clamped (see
        // kMaxSaneRuntimeSeconds).
        std::int64_t timeout = 0;
        if (auto it = input.find("timeout"); it != input.end() && it->is_number_integer() &&
                                                it->get<std::int64_t>() >= 0) {
            timeout = std::min(it->get<std::int64_t>(), kMaxSaneRuntimeSeconds);
        }
        // The request's own kill deadline: absent -> the operator's setting
        // (default unlimited); explicit 0 = unlimited; negative is ignored.
        std::int64_t max_runtime = S().background_max_runtime;
        if (auto it = input.find("max_runtime"); it != input.end() && it->is_number_integer() &&
                                                       it->get<std::int64_t>() >= 0) {
            max_runtime = std::min(it->get<std::int64_t>(), kMaxSaneRuntimeSeconds);
        }
        const bool clean = BoolField(input, "clean");

        const auto slice = ParseSlice(ctx.tool_config);
        const auto plan = MakeEnvPlan(ctx.project_root, slice, S());

        if (clean) store_.Erase(ctx.session_id, "clean restart requested");

        auto interp = store_.Find(ctx.session_id);
        std::string advisory;
        if (!interp || !interp->alive()) {
            std::string start_error;
            interp = Start(plan, ctx, &advisory, &start_error);
            if (!interp) return Error(start_error);
        } else {
            const auto diff_error = ApplyDiff(interp, plan, ctx, &advisory);
            if (!diff_error.empty()) return Error(diff_error);
        }

        const bool async = timeout == 0;
        std::string submit_error;
        std::size_t ahead = 0;
        const auto id = interp->Submit(code, async, BuildHandles(ctx.tool_use_id, code, ctx),
                                       &submit_error, max_runtime, &ahead);
        if (!id) {
            return Error(submit_error +
                         "\nThe interpreter is gone; the next python_exec starts a fresh one.");
        }
        const std::string queued =
            ahead == 0 ? "" : " Queued behind " + std::to_string(ahead) + " running/queued request(s).";

        if (async) {
            std::string ack =
                *id + " started; its result arrives as a tool result before your next reply. "
                      "python_output polls progress; python_interrupt stops it." + queued;
            if (max_runtime > 0) ack += " Deadline: " + std::to_string(max_runtime) + "s.";
            return Ok(ack + advisory);
        }

        std::int64_t wait = timeout;
        if (S().sync_wait_ceiling > 0 && wait > S().sync_wait_ceiling) {
            wait = S().sync_wait_ceiling;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
        RequestOutcome outcome;
        switch (interp->WaitSync(*id, ctx.cancel_token, deadline, &outcome)) {
            case Interpreter::WaitOutcome::kCompleted: {
                ToolResult result;
                result.is_error = !outcome.ok;
                result.content_text =
                    FormatOutcome(*id, outcome, ahead, S().max_output_bytes) + advisory;
                return result;
            }
            case Interpreter::WaitOutcome::kDemoted:
                return Ok(*id + " is still running after " + std::to_string(wait) +
                          "s; its result arrives as a tool result before your next reply. "
                          "python_output polls progress; python_interrupt stops it." + queued +
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

    // (Re)start: materialize the environment, spawn, register. For a venv
    // plan the whole sequence runs under the project install lock, so the
    // "what do other sessions depend on" check and this interpreter's
    // registration are atomic with respect to every other session's.
    std::shared_ptr<Interpreter> Start(const EnvPlan& plan, tools::ToolContext& ctx,
                                       std::string* advisory, std::string* error) {
        std::unique_lock<std::mutex> project_lock;
        std::vector<std::string> record = plan.declared;
        std::string note;
        std::vector<std::string> conflicts;
        if (plan.wants_venv) {
            project_lock = ProjectInstallMutex::Instance().Lock(plan.project_root);
            const auto shared = ResolveAgainstShared(
                plan.declared, store_.DeclaredInProject(plan.project_root, ctx.session_id));
            EnvPlan install_plan = plan;
            install_plan.declared = shared.install;
            std::string ensure_error;
            note = EnsureEnvironment(install_plan, S(), ctx.logger, &ensure_error);
            if (!ensure_error.empty()) {
                *error = "could not prepare the python environment: " + ensure_error +
                         "\nThe interpreter was not started; nothing was lost.";
                return nullptr;
            }
            record = shared.record;
            conflicts = shared.conflicts;
        }
        InterpreterConfig config;
        config.python_path = ResolveInterpreterBinary(plan);
        config.cwd = ctx.project_root;
        config.venv = plan.wants_venv ? plan.venv : std::filesystem::path{};
        config.declared = record;
        config.env = BuildChildEnv(plan, S());
        config.session_lifetime = ctx.session_lifetime;
        std::string spawn_error;
        auto interp = Interpreter::Spawn(config, S(), ctx.logger, &spawn_error);
        if (!interp) {
            *error = "failed to start the python interpreter: " + spawn_error;
            return nullptr;
        }
        store_.Add(ctx.session_id, interp);
        *advisory = "[started " + interp->version() + " in " + config.python_path +
                    (note.empty() ? "]" : "; " + note + "]");
        if (!conflicts.empty() && interp->NoticeShown("declared_conflict")) {
            *advisory += " " + ConflictNotice(conflicts);
        }
        return interp;
    }

    // The per-exec comparison (the preset switch machinery). Empty error
    // string = proceed; `advisory` gains the result-text notes.
    std::string ApplyDiff(const std::shared_ptr<Interpreter>& interp, const EnvPlan& plan,
                          tools::ToolContext& ctx, std::string* advisory) {
        const auto diff = ClassifyDiff(plan.declared, interp->Declared());
        std::string text;
        if (!interp->config().venv.empty()) {
            std::vector<std::string> conflicts;
            // Same name, different spec than THIS interpreter runs: the old
            // version is already imported here, so never re-version it.
            for (const auto& c : diff.conflicts) conflicts.push_back(c);
            if (!diff.additive.empty()) {
                const auto project_lock = ProjectInstallMutex::Instance().Lock(plan.project_root);
                const auto shared = ResolveAgainstShared(
                    diff.additive, store_.DeclaredInProject(plan.project_root, ctx.session_id));
                std::string install_error;
                const auto note = EnsurePackages(ctx.project_root, plan.venv_python,
                                                 shared.install, S(), ctx.logger,
                                                 &install_error);
                if (!install_error.empty()) {
                    return "installing the preset's newly declared packages failed: " +
                           install_error + "\nThe interpreter and its state are untouched; "
                                           "install manually or adjust the preset.";
                }
                interp->AdoptDeclared(shared.record);
                text += note.empty() ? "" : "[" + note + "]";
                for (const auto& c : shared.conflicts) conflicts.push_back(c);
            }
            if (!conflicts.empty() && interp->NoticeShown("declared_conflict")) {
                text += (text.empty() ? "" : " ") + ConflictNotice(conflicts);
            }
        } else if (plan.wants_venv) {
            // Bare interpreter, but the environment now wants the venv (the
            // preset declares packages, or .venv appeared). Materialize it for
            // the restart; the RUNNING interpreter cannot use it. One-shot per
            // interpreter: without the gate the full venv creation + install
            // re-ran on EVERY call until clean=true.
            if (interp->NoticeShown("bare_drift")) {
                const auto project_lock = ProjectInstallMutex::Instance().Lock(plan.project_root);
                const auto shared = ResolveAgainstShared(
                    plan.declared, store_.DeclaredInProject(plan.project_root, ctx.session_id));
                EnvPlan install_plan = plan;
                install_plan.declared = shared.install;
                std::string ensure_error;
                EnsureEnvironment(install_plan, S(), ctx.logger, &ensure_error);
                if (!ensure_error.empty()) {
                    ctx.logger.warn("python[{}]: venv materialization failed: {}", ctx.session_id,
                                    ensure_error);
                }
                text += "[this interpreter runs system python; the project venv now exists -- "
                        "python_exec(clean=true) to move into it]";
            }
        }
        *advisory += text.empty() ? "" : " " + text;
        return {};
    }

    // The current settings: a reconfigure swaps them between calls.
    PythonSettings S() const { return *live_->Get(); }
    std::shared_ptr<const common::LiveValue<PythonSettings>> live_;
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
        const bool kill = BoolField(input, "kill");
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
        // Captured BEFORE SoftInterrupt, not after: see ProgressSnapshot's
        // comment for why the order matters.
        const auto baseline = interp->Progress();
        interp->SoftInterrupt();
        switch (interp->WaitForInterrupt(baseline, std::chrono::seconds(settings_.grace_seconds),
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

std::unique_ptr<tools::Tool> MakePythonExecTool(std::shared_ptr<const common::LiveValue<PythonSettings>> live,
                                                InterpreterStore& store) {
    return std::make_unique<PythonExecTool>(std::move(live), store);
}

std::unique_ptr<tools::Tool> MakePythonExecTool(PythonSettings settings, InterpreterStore& store) {
    return MakePythonExecTool(
        std::make_shared<common::LiveValue<PythonSettings>>(std::move(settings)), store);
}

// --- python_output -----------------------------------------------------------------

namespace {

class PythonOutputTool : public tools::Tool {
public:
    PythonOutputTool(PythonSettings settings, InterpreterStore& store)
        : settings_(std::move(settings)), store_(store) {
        spec_.name = "python_output";
        // Pure read of the plugin's own buffers: nothing runs, nothing is
        // written -- polling must not cost a permission prompt.
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = "python_output";
        spec_.definition.description =
            "Poll background work in this session's Python interpreter without blocking.\n"
            "No arguments (or scope='all'): one status line per task -- running (age, live "
            "output size), queued behind N, or finished (ok/failed, runtime).\n"
            "task_id + scope: that task's output. scope is 'top:N', 'tail:N', or "
            "'read:OFFSET,LENGTH' -- byte-offset slices of the task's output stream (a running "
            "task's live ring; a finished task's final stdout capture). Responses carry the "
            "stream total and the next read for paging; output that fell out of a running "
            "task's ring comes back as an explicit '[... N bytes lost ...]' marker.\n"
            "grep (optional regex): filters the sliced text line-by-line; each match is "
            "prefixed with its byte offset in the stream (usable as a read: offset).";
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
                                "(default: tail:8192), 'read:OFFSET,LENGTH'."}}},
              {"grep",
               {{"type", "string"},
                {"description", "Regex applied to the sliced output's lines after scoping; "
                                "matches report their stream byte offsets."}}}}}};
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
        const auto task_id = StringField(args, "task_id");
        const auto scope = StringField(args, "scope");
        const auto grep = StringField(args, "grep");

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
                                Seconds(t.seconds);
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
            // 'read:OFFSET,LENGTH' is documented; 'read:OFFSET:LENGTH' is the
            // obvious misspelling and means the same thing.
            const auto sep = scope.find_first_of(",:", 5);
            if (sep == std::string::npos) {
                return Error("scope 'read:' takes 'read:<offset>,<length>'");
            }
            offset = ParseCount(scope.substr(5, sep - 5), 0);
            length = static_cast<std::size_t>(ParseCount(scope.substr(sep + 1), 0));
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
            // RE2, not std::regex: `grep` is model-authored, and RE2 is the
            // linear-time guarantee that decision rests on (see
            // tools-search/CMakeLists.txt). std::regex offers neither a
            // linear-time bound (catastrophic backtracking on a pattern like
            // `(a+)+$`) nor a bounded call stack (both libstdc++ and libc++
            // implement matching recursively), and this method's own
            // try/catch only ever caught regex_error -- it caught neither.
            const re2::RE2 matcher(grep);
            if (!matcher.ok()) {
                return Error("bad grep regex: " + matcher.error());
            }
            std::string filtered;
            std::size_t pos = 0;
            while (pos < body.size()) {
                const auto nl = body.find('\n', pos);
                const auto line_end = nl == std::string::npos ? body.size() : nl + 1;
                const std::string_view line(body.data() + pos, line_end - pos);
                if (re2::RE2::PartialMatch(re2::StringPiece(line.data(), line.size()), matcher)) {
                    // Stream offsets, not slice offsets: directly usable as
                    // a read: offset (exact unless a gap marker precedes).
                    filtered += "[" + std::to_string(offset + pos) + "] " + std::string(line);
                    if (filtered.back() != '\n') filtered += "\n";
                }
                pos = line_end;
            }
            body = filtered.empty() ? "(no matches)" : filtered;
        }
        if (body.empty()) body = "(no output in this range)";
        text += "\n" + body;
        if (read.slice.next < read.slice.total) {
            const auto page = length > 0 ? length : kDefaultTailBytes;
            text += "\n[next: read:" + std::to_string(read.slice.next) + "," +
                    std::to_string(page) + "]";
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
