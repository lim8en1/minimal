#pragma once

// The sinks a Session hands its loop and tools, built in one place with every
// dependency explicit.
//
// The point is the CAPTURE DISCIPLINE, which used to be ambient in a 190-line
// constructor. Two kinds of callable come out of here:
//
//   * OUTLIVING sinks -- injection_sink, task_event_sink, tool_output_sink,
//     compaction_sink, session_lifetime. A background watcher or a detached
//     thread keeps a copy past the tool call, the turn and the Session itself,
//     so they capture ONLY by-value shared state (the channels block, the event
//     sink, ids) -- never the Session.
//   * SESSION-BOUND callbacks -- tool_lifecycle_sink, steer_gate, the stream
//     sink. They run on the worker while the Session lives and reach back into
//     it through the std::function members of SessionSinkDeps, which the
//     Session binds to itself.

#include <functional>
#include <memory>
#include <string>

#include "minimal/agent/agent_loop.hpp"
#include "minimal/agent/session_channels.hpp"
#include "minimal/agent/session_event.hpp"
#include "minimal/agent/session_observer.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/tools/prompt.hpp"
#include "minimal/tools/tool_executor.hpp"

namespace minimal::agent {

struct SessionSinkDeps {
    common::Logger& logger;
    std::string session_id;
    std::string parent_session_id;  // empty for a top-level session
    std::filesystem::path project_root;

    std::shared_ptr<SessionChannels> channels;
    SessionEventSink event_sink;
    SessionObserverRegistry& observers;
    const hooks::HookDispatcher* hooks = nullptr;
    session::HistoryWindow* history = nullptr;  // null without a SQLite-backed reader
    tools::PromptBroker* prompt_broker = nullptr;

    // Session-bound (see the header comment); must stay valid for the Session's life.
    std::function<bool()> is_cancelled;
    std::function<void()> wake_worker;
    std::function<SessionInfo()> info_snapshot;
    std::function<tools::ToolExecutor::HookLabels(const std::string&, const nlohmann::json&)>
        hook_labels;
};

struct SessionSinks {
    SessionContext context;
    llm::StreamEventSink stream_event_sink;
};

SessionSinks BuildSessionSinks(const SessionSinkDeps& deps);

}  // namespace minimal::agent
