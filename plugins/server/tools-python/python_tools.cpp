#include "python_tools.hpp"

#include <algorithm>
#include <chrono>
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
            "Run code in this session's persistent Python interpreter (state survives across "
            "calls and turns: globals, imports, live connections).\n"
            "timeout (seconds, default 0): 0 runs ASYNC -- returns 'py-N (pid P)' immediately and "
            "the result is delivered later; there is no deadline, so this is the mode for "
            "long-running or fire-and-forget code. timeout > 0 waits synchronously up to that "
            "many seconds; if the code is still running at the deadline, the wait converts to "
            "async and the result still arrives later -- the interpreter is never killed by a "
            "timeout.\n"
            "clean (default false): kill the current interpreter and run in a fresh one. This is "
            "the only way state is lost, and it is your explicit choice. Also the recovery for a "
            "wedged interpreter (e.g. a stuck non-interruptible C call): pair it with "
            "python_interrupt(kill=true) knowledge.\n"
            "A long-running async request occupies the interpreter: later requests queue behind "
            "it (reported as 'queued behind N'). Use python_interrupt to stop one.\n"
            "The environment is shared per-project: <project_root>/.venv, created on demand when "
            "the agent preset declares packages. All times are SECONDS.";
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
                 "Seconds to wait for a result. 0 (default) = async: return immediately, "
                 "result delivered later. >0 = wait up to N seconds, then convert to async."}}},
              {"clean",
               {{"type", "boolean"},
                {"description", "Restart the interpreter before running: all previous state is "
                                "discarded (explicit, and the only way state is lost)."}}}}},
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
        const auto id = interp->Submit(code, async, std::move(handles), &submit_error);
        if (!id) {
            return Error(submit_error +
                         "\nThe interpreter is gone; the next python_exec starts a fresh one.");
        }

        if (async) {
            EmitStarted(BuildHandles(ctx.tool_use_id, code, ctx), *id, interp->pid());
            return Ok(*id + " started in the background (pid " + std::to_string(interp->pid()) +
                      "); the result will be delivered when it completes. python_interrupt stops "
                      "it." + advisory);
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
                          "s; it continues in the interpreter and its result will be delivered "
                          "when it completes. python_interrupt stops it." + advisory);
            case Interpreter::WaitOutcome::kCancelled:
                return Ok(*id + " was cancelled while waiting; it continues in the interpreter "
                                "and its result will be delivered when it completes." +
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

std::unique_ptr<tools::Tool> MakePythonInterruptTool(PythonSettings settings,
                                                     InterpreterStore& store) {
    return std::make_unique<PythonInterruptTool>(std::move(settings), store);
}

}  // namespace minimal::tools_python
