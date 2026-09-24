#include "minimal/agent/session_launch.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <utility>

#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/signal_router.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/mcp/signal_protocol.hpp"
#include "minimal/tools/permission_advisor.hpp"

namespace minimal::agent {
namespace {

using Code = LaunchError::Code;

// SplitModelRef, not ParseModelRef: a client may name a discovered model whose
// id itself carries slashes ("openrouter/anthropic/claude-opus-5"). Shape-
// checked here so a malformed reference is INVALID_ARGUMENT rather than a
// precondition failure from deep inside session construction.
std::optional<common::ModelRef> RequestedModel(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    auto model = common::SplitModelRef(raw);
    if (!model) {
        throw LaunchError(Code::kInvalidArgument,
                          "model '" + raw + "' must be <provider>/<model>, e.g. local/qwen4");
    }
    return model;
}

// Settings that are properties of the deployment, not of any stored row:
// applied the same on creation and on resume.
void ApplyDeploymentSettings(const LaunchContext& context, SessionParams& params) {
    params.mcp_manager = context.mcp_manager;
    params.wake_on_injection = context.settings.background_wake;
    params.pause_auto_resume_on_submit = context.settings.pause_auto_resume_on_submit;
}

// Records permissions.json's configured advisor as a SELECTION on params;
// the session wiring step composes it. No advisor configured, or the named
// plugin not loaded (a runtime condition the sanity check warns about):
// nothing selected, and every prompt reaches the human.
void SelectAdvisor(const LaunchContext& context, SessionParams& params) {
    if (!context.permissions.advisor || context.advisors == nullptr) return;
    params.selected_advisor = context.advisors->Find(*context.permissions.advisor);
    params.advisor_timeout = std::chrono::milliseconds(context.permissions.advisor_timeout_ms);
}

// Validates every selector and warms every named channel BEFORE the session
// exists: a typo in one of five selectors must refuse the start, not quietly
// deliver four of five signals, and a channel that cannot connect would
// otherwise look exactly like "connected but no signals ever arrive".
void ApplySignals(const LaunchContext& context, const std::vector<std::string>& signals,
                  SessionParams& params) {
    if (signals.empty()) return;
    if (context.signal_router == nullptr) {
        throw LaunchError(Code::kInvalidArgument,
                          "signals requested but this server does not offer them");
    }
    std::vector<std::string> channels;
    for (const auto& selector : signals) {
        std::string error;
        std::string channel;
        if (!context.signal_router->CheckSelector(selector, error, &channel)) {
            throw LaunchError(Code::kInvalidArgument, error);
        }
        // The internal source has no connection to warm. A null manager (a
        // host that wires the router without live MCP, e.g. a test) keeps
        // validation structural-only, like an internal-source selector.
        if (channel != mcp::kInternalSignalSource && context.mcp_manager != nullptr) {
            std::string connect_error;
            if (!context.mcp_manager->EnsureAlive(channel, connect_error)) {
                throw LaunchError(Code::kFailedPrecondition, "signal channel '" + channel +
                                                                 "' is not available: " +
                                                                 connect_error);
            }
        }
        if (std::find(channels.begin(), channels.end(), channel) == channels.end()) {
            channels.push_back(channel);
        }
    }
    // A subscribed session exists to react to signals: it wakes on injection,
    // whatever background_wake says about subagent completions.
    params.wake_on_injection = true;
    params.signal_context = context.signal_router->SignalContextFor(signals);
    // Staged, not acted on: SessionRegistry registers the subscriptions
    // atomically with the session becoming findable.
    params.signal_router = context.signal_router;
    params.signal_selectors = signals;
}

// project_root is the confinement boundary and bash's initial cwd, so a bad
// value fails fast with a reason rather than surfacing later as confusing tool
// errors. faccessat with AT_EACCESS, not access(2): the answer must be for the
// server's EFFECTIVE uid.
void ValidateProjectRoot(const std::filesystem::path& root) {
    if (root.empty()) {
        // Client-supplied and per-session: defaulting it server-side would
        // silently point the agent at the server's own cwd.
        throw LaunchError(Code::kInvalidArgument, "project_root is required");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        throw LaunchError(Code::kFailedPrecondition,
                          "project_root is not an existing directory: " + root.string());
    }
    if (::faccessat(AT_FDCWD, root.c_str(), W_OK, AT_EACCESS) != 0) {
        throw LaunchError(Code::kFailedPrecondition,
                          "project_root is not writable: " + root.string());
    }
}

// A store read that throws (locked database, I/O error, corruption) is an
// INTERNAL failure, never "no such session".
std::optional<session::SessionSummary> ReadRow(const LaunchContext& context,
                                               const std::string& session_id) {
    try {
        return context.db.GetSession(session_id);
    } catch (const std::exception& e) {
        throw LaunchError(Code::kInternal, e.what());
    }
}

session::SessionSummary Create(const LaunchContext& context, const LaunchRequest& request) {
    SessionParams params;
    // Passed through EMPTY when the client sent nothing, never defaulted to a
    // placeholder: the database names an unnamed session from its first user
    // message, guarded by `name = ''`, and a placeholder would satisfy that
    // guard and permanently suppress the default naming.
    params.name = request.name;
    params.agent_preset = request.agent_preset.empty() ? context.settings.default_agent_preset
                                                       : request.agent_preset;
    params.project_root = request.project_root;
    // Unset means "the client did not ask": the run-mode default is applied
    // with the rest of permissions.json, in AgentPresetRegistry.
    params.run_mode_override = request.run_mode;
    ApplyDeploymentSettings(context, params);
    params.model_override = RequestedModel(request.model);

    if (params.agent_preset.empty()) {
        throw LaunchError(Code::kFailedPrecondition,
                          "no agent_preset given and settings.json sets no default_agent_preset");
    }
    ValidateProjectRoot(params.project_root);

    // The DATABASE mints the id, so the row exists before the Session does:
    // it is the only party that can guarantee uniqueness across restarts, and
    // the transcript must be attributable to a row from its first append.
    session::SessionSummary row;
    try {
        row = context.db.CreateSession(
            params.name, std::nullopt, params.project_root.string(), params.agent_preset,
            params.model_override ? std::optional<std::string>(params.model_override->str())
                                  : std::nullopt);
    } catch (const std::exception& e) {
        context.logger.warn("CreateSession: could not record session: {}", e.what());
        throw LaunchError(Code::kInternal, e.what());
    }
    params.session_id = row.session_id;

    // Every later failure rolls the just-minted row back: inserting first is
    // what makes the id authoritative, but a message-less session must not sit
    // in the picker forever. A rollback failure is logged, never allowed to
    // shadow the error that caused it.
    const auto rollback = [&](const char* what) {
        try {
            context.db.DeleteSession(row.session_id);
        } catch (const std::exception& e) {
            context.logger.warn("CreateSession: rollback after {} failed: {}", what, e.what());
        }
    };

    SelectAdvisor(context, params);
    try {
        ApplySignals(context, request.signals, params);
    } catch (const LaunchError&) {
        rollback("signal refusal");
        throw;
    }

    try {
        context.sessions.Create(std::move(params));
    } catch (const std::exception& e) {
        // Preset and provider problems are configuration errors the operator
        // has to see and fix, so they come back with their reason attached.
        rollback("construction failure");
        context.logger.warn("CreateSession failed: {}", e.what());
        throw LaunchError(Code::kFailedPrecondition, e.what());
    }
    return row;
}

session::SessionSummary Resume(const LaunchContext& context, const LaunchRequest& request) {
    // The stored row governs name, agent_preset and project_root: a resumed
    // session keeps the identity and confinement boundary it was created
    // with. Rejecting a conflicting field beats silently ignoring it.
    // run_mode is the exception -- a property of the CLIENT, not the session.
    if (!request.name.empty() || !request.agent_preset.empty() || !request.project_root.empty()) {
        throw LaunchError(Code::kInvalidArgument,
                          "resume carries session_id (and run_mode) only; the stored session "
                          "keeps its name, agent_preset, and project_root");
    }
    const auto stored = ReadRow(context, request.session_id);
    if (!stored) {
        throw LaunchError(Code::kNotFound, "no such session: " + request.session_id);
    }
    // A subagent's life was one turn of its parent's; resuming one standalone
    // has no meaning.
    if (stored->parent_session_id) {
        throw LaunchError(Code::kFailedPrecondition,
                          "subagent sessions cannot be resumed: " + request.session_id);
    }
    // Two live Sessions on one transcript would interleave appends.
    if (context.sessions.Find(stored->session_id) != nullptr) {
        throw LaunchError(Code::kAlreadyExists,
                          "session '" + stored->session_id + "' is already open");
    }

    SessionParams params;
    params.session_id = stored->session_id;
    params.name = stored->name;
    params.agent_preset = stored->agent_preset;
    params.project_root = stored->project_root;
    params.run_mode_override = request.run_mode;
    params.resume = true;
    ApplyDeploymentSettings(context, params);
    SelectAdvisor(context, params);
    // A model named on THIS request outranks the stored one; with none, the
    // model the session was created (or last /model-ed) with is restored, so
    // a restart never silently reverts to the preset default. Same for the
    // subagent-model choice.
    const auto request_model = RequestedModel(request.model);
    params.model_override = request_model;
    if (!params.model_override && stored->model) {
        params.model_override = common::SplitModelRef(*stored->model);
    }
    if (stored->subagent_model) {
        params.subagent_model_override = common::SplitModelRef(*stored->subagent_model);
    }
    // Signals on resume follow the creation rules; never persisted from the
    // previous stream.
    ApplySignals(context, request.signals, params);

    try {
        // Loads the stored transcript, repairs a torn tool-call tail if there
        // is one, and starts the session.
        context.sessions.Create(std::move(params));
    } catch (const SessionAlreadyOpen& e) {
        // The loser of a race between two resumes: the same condition the
        // pre-check above reports, so the same status.
        context.logger.warn("resume of session {} lost the open race", stored->session_id);
        throw LaunchError(Code::kAlreadyExists, e.what());
    } catch (const std::exception& e) {
        // NO ROLLBACK: the row is the user's existing session. The failure
        // (the stored preset no longer resolves, its provider kind is gone) is
        // reported and the transcript survives.
        context.logger.warn("resume of session {} failed: {}", stored->session_id, e.what());
        throw LaunchError(Code::kFailedPrecondition, e.what());
    }

    // The request's model now governs; make the row say so, so the NEXT
    // model-less resume keeps this choice. Only after construction succeeded,
    // and best-effort like the runtime persistence.
    if (request_model) {
        try {
            context.db.UpdateSessionModel(stored->session_id, request_model->str());
        } catch (const std::exception& e) {
            context.logger.warn("resume of session {}: persisting model override failed: {}",
                                stored->session_id, e.what());
        }
    }

    // Re-read: resume may have appended a repair message, and the caller
    // reports the current message_count.
    const auto fresh = ReadRow(context, stored->session_id);
    if (!fresh) {
        throw LaunchError(Code::kInternal,
                          "session row vanished during resume: " + stored->session_id);
    }
    return *fresh;
}

}  // namespace

session::SessionSummary Launch(const LaunchContext& context, const LaunchRequest& request) {
    return request.session_id.empty() ? Create(context, request) : Resume(context, request);
}

session::SessionSummary Fork(const LaunchContext& context, const ForkRequest& request) {
    if (request.source_session_id.empty()) {
        throw LaunchError(Code::kInvalidArgument, "source_session_id is required");
    }
    // Shape-checked here, not at the worker, so a malformed ref never
    // survives to the queue.
    std::optional<common::ModelRef> model;
    if (!request.model.empty()) {
        model = common::SplitModelRef(request.model);
        if (!model) {
            throw LaunchError(Code::kInvalidArgument, "model must be \"<provider>/<model>\" (got \"" +
                                                          request.model + "\")");
        }
    }
    std::optional<std::string> agent_preset;
    if (!request.agent_preset.empty()) agent_preset = request.agent_preset;

    std::shared_ptr<Session> session;
    try {
        session = context.sessions.ForkSession(request.source_session_id, request.target_seq,
                                               request.name, model, agent_preset, request.run_mode);
    } catch (const std::exception& e) {
        // Store refusals (no such top-level session, invalid target) and
        // preset/model resolution failures alike; the registry already rolled
        // a minted row back on the latter.
        context.logger.warn("ForkSession failed: {}", e.what());
        throw LaunchError(Code::kFailedPrecondition, e.what());
    }
    // The persisted row carries message_count and fork provenance the
    // in-memory info does not.
    const auto row = ReadRow(context, session->info().session_id);
    if (!row) throw LaunchError(Code::kInternal, "forked session row vanished");
    return *row;
}

}  // namespace minimal::agent
