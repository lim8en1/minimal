#include "windbg_tools.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "minimal/sidecar/lifetime_watch.hpp"
#include "minimal/sidecar/task_registry.hpp"

namespace minimal::tools_windbg {

namespace {

using tools::PermissionDisclosure;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

// The lifetime-safe handles a background job's watcher needs, captured per
// invocation (the shared TaskRegistry contract).
sidecar::TaskRegistry::WatchHandles BuildHandles(const std::string& label,
                                                 const ToolContext& ctx) {
    sidecar::TaskRegistry::WatchHandles handles;
    handles.emit_output = ctx.emit_output;
    handles.schedule_injection = ctx.schedule_injection;
    handles.emit_task_event = ctx.emit_task_event;
    handles.session_lifetime = ctx.session_lifetime;
    handles.tool_use_id = ctx.tool_use_id;
    handles.logger = &ctx.logger;
    (void)label;
    return handles;
}

std::string DescribeState(const nlohmann::json& state) {
    if (!state.is_object()) return {};
    const auto kind = state.value("state", std::string("unknown"));
    std::string out = "target: " + kind;
    if (state.contains("stop_reason") && state.at("stop_reason").is_string()) {
        out += " (" + state.at("stop_reason").get<std::string>() + ")";
    }
    return out;
}

ToolResult Ok(const std::string& text) { return ToolResult{false, text}; }
ToolResult Error(const std::string& text) { return ToolResult{true, text}; }

// The verdict line dbgd appends to a job's transcript; presence flips the
// injected result to an error. (The server owns the verdict -- "ok is an
// HRESULT verdict, never pattern-matched output" applies to engine calls;
// the job transcript's trailer is the exec sequence's own last word.)
constexpr std::string_view kExecFailedMarker = "[exec failed]";

// The background watcher for a job that outlived its tool call: long-poll
// the transcript, stream chunks live, and report the verdict at a turn
// boundary. Runs detached under the shared TaskRegistry's containment
// wrapper; session death ends it (the remote session is closed by the
// store's own lifetime watcher, nobody here has to).
void WatchJob(sidecar::TaskRegistry::TaskState& task, WindbgSettings settings,
              const std::string& handle, const std::string& job_id, std::string partial,
              std::int64_t since, const sidecar::TaskRegistry::WatchHandles& handles) {
    DbgdClient client(std::move(settings));
    // `partial` is the transcript already collected by the (now-demoted) sync
    // wait -- the watcher owns it and the cursor continues from `since`.
    std::string text = std::move(partial);
    std::int64_t offset = since;
    while (true) {
        if (sidecar::TaskRegistry::SessionGone(handles)) {
            sidecar::TaskRegistry::Finish(task, handles,
                                          tools::BackgroundTaskEvent::State::kKilled, "",
                                          false);
            return;
        }
        DbgdError error;
        auto chunk = client.ReadJob(handle, job_id, offset, &error);
        if (!error.code.empty()) {
            // Transport loss, protocol mismatch, or the remote session is
            // gone: report honestly -- "session_gone" means the next windbg
            // call should reconnect, everything else means stop retrying.
            std::string reason = error.message.empty() ? error.code : error.message;
            if (!error.hint.empty()) reason += "\nhint: " + error.hint;
            sidecar::TaskRegistry::Finish(task, handles,
                                          tools::BackgroundTaskEvent::State::kFailed, reason,
                                          true);
            return;
        }
        if (!chunk.output.empty()) {
            text += chunk.output;
            // Live channel to the operator, keyed to the original
            // tool_use_id; the injected text below is the record.
            if (handles.emit_output) handles.emit_output(chunk.output);
            offset = chunk.next_offset;
        }
        if (chunk.done) break;
    }

    const bool failed = text.find(kExecFailedMarker) != std::string::npos;
    sidecar::TaskRegistry::Finish(task, handles,
                                  failed ? tools::BackgroundTaskEvent::State::kFailed
                                         : tools::BackgroundTaskEvent::State::kCompleted,
                                  text, failed);
}

// Collects a job synchronously on the session worker up to `deadline`,
// mirroring python_exec's WaitSync: turn cancellation DEMOTES to background
// (the job continues; the watcher injects later), it never abandons it.
enum class CollectOutcome { kDone, kDemote, kError };
CollectOutcome CollectSync(DbgdClient& client, const std::string& handle,
                           const std::string& job_id, std::int64_t* offset,
                           std::string* partial,
                           std::chrono::steady_clock::time_point deadline,
                           const tools::CancelTokenPtr& cancel, DbgdError* error) {
    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = client.ReadJob(handle, job_id, *offset, error);
        if (!error->code.empty()) {
            *partial += chunk.output;  // whatever arrived before the failure
            return CollectOutcome::kError;
        }
        if (!chunk.output.empty()) {
            *partial += chunk.output;
            *offset = chunk.next_offset;
        }
        if (chunk.done) return CollectOutcome::kDone;
        if (cancel && cancel->IsCancelled()) return CollectOutcome::kDemote;
        // The JobRead long-poll already blocks server-side for job_wait_s;
        // no client-side sleep is needed.
    }
    return CollectOutcome::kDemote;  // budget spent: demote
}

// --- windbg ---------------------------------------------------------------------------

class WindbgTool : public tools::Tool {
public:
    WindbgTool(WindbgSettings settings, DbgdClient& client, WindbgSessionStore& store,
               sidecar::TaskRegistry& jobs)
        : settings_(std::move(settings)),
          client_(client),
          store_(store),
          jobs_(jobs) {
        spec_.name = "windbg";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Run a command in the WinDbg debugger session attached to this harness session "
            "(a dbgd server owns the engine). The session is created on first use and "
            "persists across calls; the target's state survives.\n"
            "Fast commands answer directly; slow ones (resumes, long traces) return a task "
            "id and the result is reported automatically at a later turn boundary -- the "
            "call never blocks on a running target. timeout (seconds, default 0): >0 waits "
            "synchronously up to that many seconds, then converts to background delivery "
            "(the command is never killed by a timeout).\n"
            "target (first call only): {\"kind\": \"exec\"|\"pid\"|\"dump\"|\"kernel\", ...} "
            "spec -- without it, the configured default_target is used. Use windbg_interrupt "
            "to break into a running target.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"command",
               {{"type", "string"},
                {"description", "Debugger command to run, e.g. 'k', '~* k', 'lm', 'bp ...'"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Seconds to wait for a result. 0 (default) = background: "
                                "fast commands still answer immediately, running ones return "
                                "a task id. >0 = wait up to N seconds, then convert to "
                                "background delivery."}}},
              {"allow_running",
               {{"type", "boolean"},
                {"description", "Permit the command even though the target is running "
                                "(commands valid against a live target)"}}},
              {"target",
               {{"type", "object"},
                {"description", "dbgd TargetSpec for the FIRST connect: {\"kind\": "
                                "\"exec\"|\"pid\"|\"dump\"|\"kernel\", ...}. Later calls "
                                "ignore it."}}}}},
            {"required", nlohmann::json::array({"command"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "windbg"; }
    bool parallel_safe() const override { return false; }
    bool network_access_required() const override { return true; }

    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const ToolContext&) const override {
        PermissionDisclosure disclosure;
        disclosure.summary = "windbg: " + input.value("command", std::string("<invalid>"));
        disclosure.complete = true;
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        const auto command_it = input.find("command");
        if (command_it == input.end() || !command_it->is_string() ||
            command_it->get<std::string>().empty()) {
            return Error("windbg requires a non-empty 'command' string");
        }
        const auto command = command_it->get<std::string>();
        const bool allow_running = input.value("allow_running", false);
        std::int64_t timeout = 0;
        if (auto it = input.find("timeout");
            it != input.end() && it->is_number_integer() && it->get<std::int64_t>() > 0) {
            timeout = it->get<std::int64_t>();
        }

        // --- Session: find or create (first connect) ------------------------
        auto session = store_.Find(ctx.session_id);
        if (!session) {
            const auto target = ResolveTarget(input);
            if (target.is_null()) {
                return Error(
                    "no debugger session yet and no target given: pass a `target` object "
                    "(dbgd TargetSpec, e.g. {\"kind\": \"pid\", \"value\": 4242} or "
                    "{\"kind\": \"kernel\", \"connection\": \"net:port=50000,key=...\"}) or "
                    "configure the plugin's default_target");
            }
            std::string error_text;
            const auto handle = Connect(target, &error_text);
            if (handle.empty()) return Error(error_text);

            session = std::make_shared<WindbgSession>();
            session->handle = handle;
            session->target_text = target.dump();
            session->closed = std::make_shared<std::atomic<bool>>(false);
            // The remote session must die when the harness session does, and
            // there is no synchronous teardown hook -- so the cleanup is
            // armed on the sidecar's lifetime watcher and disarmed by the
            // store's explicit Erase. Both paths are idempotent through the
            // `closed` flag.
            const std::string handle_copy = handle;
            const std::string on_exit = settings_.on_session_end;
            session->lifetime_guard = sidecar::WatchLifetime(
                ctx.session_lifetime,
                [session, client = &client_, handle_copy, on_exit] {
                    bool expected = false;
                    if (session->closed->compare_exchange_strong(expected, true)) {
                        client->Close(handle_copy, on_exit);
                    }
                });
            store_.Add(ctx.session_id, session);
            ctx.logger.info("windbg[{}] connected session {} ({})", ctx.session_id, handle,
                            session->target_text);
        }

        // --- Exec: fast commands answer, running ones come back as jobs ----
        DbgdError error;
        auto exec = client_.Exec(session->handle, command, allow_running, &error);
        if (!error.code.empty()) {
            return Error(FormatError(error, "windbg exec failed"));
        }
        if (exec.job_id.empty()) {
            // Synchronous shape: the sequence finished within the grace
            // window. The transcript leads every failure -- it is usually
            // what the model needs (the error, the partial output).
            std::string text = exec.output;
            if (!exec.ok || !exec.error_text.empty()) {
                if (!text.empty()) text += "\n";
                text += "[error] " + exec.error_text;
                return ToolResult{true, text + Footer(exec.state)};
            }
            return Ok(text.empty() ? "(no output)" : text + Footer(exec.state));
        }

        // --- Job shape ------------------------------------------------------
        std::int64_t offset = 0;
        std::string partial = exec.output;
        if (timeout > 0) {
            std::int64_t wait = timeout;
            if (settings_.sync_wait_ceiling > 0 && wait > settings_.sync_wait_ceiling) {
                wait = settings_.sync_wait_ceiling;
            }
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(wait);
            DbgdError collect_error;
            switch (CollectSync(client_, session->handle, exec.job_id, &offset, &partial,
                                deadline, ctx.cancel_token, &collect_error)) {
                case CollectOutcome::kDone:
                    return Ok(partial.empty() ? "(no output)" : partial);
                case CollectOutcome::kError:
                    return Error(FormatError(collect_error, "windbg exec failed") + "\n" +
                                 partial);
                case CollectOutcome::kDemote:
                    break;  // budget spent or turn cancelled: the watcher delivers
            }
        }
        return StartBackground(ctx, session->handle, exec.job_id, command,
                               std::move(partial), offset);
    }

private:
    // The ack. pid is 0 (the task is a remote job, not a process); the task
    // id is what /tasks and the model use.
    ToolResult StartBackground(ToolContext& ctx, const std::string& handle,
                               const std::string& job_id, const std::string& command,
                               std::string partial, std::int64_t since) {
        if (!ctx.schedule_injection) {
            return Error("background debugger jobs are unavailable in this session "
                         "(no deferred-completion channel)");
        }
        sidecar::TaskRegistry::StartArgs args;
        args.session_id = ctx.session_id;
        args.label = command;
        args.pid = 0;
        args.id_prefix = "wdbg";
        args.max_running = static_cast<std::size_t>(settings_.max_jobs);
        args.handles = BuildHandles(command, ctx);

        // The watcher owns its own client (stateless transport, one per
        // call) and takes the partial transcript collected before the
        // demotion. Everything captured is owned -- the settings copy, the
        // strings -- and the registry guarantees the TaskState outlives it.
        const std::string handle_copy = handle;
        const std::string job_copy = job_id;
        args.body = [settings = settings_, handle_copy, job_copy,
                     partial = std::move(partial), since](sidecar::TaskRegistry::TaskState& task,
                                                          const sidecar::TaskRegistry::WatchHandles&
                                                              watch_handles) {
            WatchJob(task, settings, handle_copy, job_copy, std::move(partial), since,
                     watch_handles);
        };

        std::string error;
        const auto id = jobs_.Start(std::move(args), &error);
        if (!id) return Error(error);

        return Ok("running in the debugger as task " + *id + " (job " + job_id +
                  "); the result will be reported automatically at a turn boundary "
                  "once it finishes. windbg_interrupt breaks into a running target; "
                  "this session ends, so does the remote session (on_exit: " +
                  settings_.on_session_end + ").");
    }

    std::string Footer(const nlohmann::json& state) const {
        const auto described = DescribeState(state);
        return described.empty() ? "" : "\n" + described;
    }

    std::string Connect(const nlohmann::json& target, std::string* error_text) {
        DbgdError error;
        const auto handle = client_.Connect(target, &error);
        if (!error.code.empty() || handle.empty()) {
            *error_text = FormatError(error, "windbg connect failed");
            return {};
        }
        return handle;
    }

    nlohmann::json ResolveTarget(const nlohmann::json& input) const {
        if (input.contains("target") && input.at("target").is_object()) {
            return input.at("target");
        }
        return settings_.default_target;
    }

    static std::string FormatError(const DbgdError& error, const std::string& fallback) {
        std::string text =
            error.message.empty() ? (fallback + ": " + error.code) : (fallback + ": " + error.message);
        if (!error.hint.empty()) text += "\nhint: " + error.hint;
        if (error.code == "session_gone") {
            text += "\nThe debugger session is gone; the next windbg call reconnects.";
        } else if (error.code == "target_running") {
            text += "\nThe target is running; windbg_interrupt breaks in, or pass "
                    "allow_running=true for commands valid against a live target.";
        }
        return text;
    }

    ToolSpec spec_;
    WindbgSettings settings_;
    DbgdClient& client_;
    WindbgSessionStore& store_;
    sidecar::TaskRegistry& jobs_;
};

// --- windbg_interrupt ------------------------------------------------------------------

class WindbgInterruptTool : public tools::Tool {
public:
    WindbgInterruptTool(WindbgSettings settings, DbgdClient& client, WindbgSessionStore& store)
        : settings_(std::move(settings)), client_(client), store_(store) {
        spec_.name = "windbg_interrupt";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Break into the running debuggee (Ctrl-C equivalent). Deliberately NOT serialized "
            "behind a busy command: this is the operation that has to work while one is in "
            "flight. Reports the resulting target state.";
        spec_.definition.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    }

    const ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return "windbg"; }
    bool parallel_safe() const override { return false; }
    bool network_access_required() const override { return true; }

    ToolResult execute(const nlohmann::json&, ToolContext& ctx) override {
        const auto session = store_.Find(ctx.session_id);
        if (!session) {
            return ToolResult{true, "no debugger session; nothing to interrupt"};
        }
        DbgdError error;
        const auto payload = client_.Interrupt(session->handle, &error);
        if (!error.code.empty()) {
            return ToolResult{true, error.message.empty() ? error.code : error.message};
        }
        const auto state =
            payload.contains("state") ? payload.at("state") : nlohmann::json{};
        return ToolResult{false, "interrupt sent; " + DescribeState(state)};
    }

private:
    ToolSpec spec_;
    WindbgSettings settings_;
    DbgdClient& client_;
    WindbgSessionStore& store_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeWindbgTool(WindbgSettings settings, DbgdClient& client,
                                            WindbgSessionStore& store,
                                            sidecar::TaskRegistry& jobs) {
    return std::make_unique<WindbgTool>(std::move(settings), client, store, jobs);
}

std::unique_ptr<tools::Tool> MakeWindbgInterruptTool(WindbgSettings settings, DbgdClient& client,
                                                     WindbgSessionStore& store) {
    return std::make_unique<WindbgInterruptTool>(std::move(settings), client, store);
}

}  // namespace minimal::tools_windbg
