#include "minimal/agent/session_sinks.hpp"

#include <utility>

#include "minimal/common/utf8.hpp"

namespace minimal::agent {

SessionSinks BuildSessionSinks(const SessionSinkDeps& deps) {
    SessionSinks out{SessionContext{deps.logger}, {}};
    SessionContext& context = out.context;
    context.session_id = deps.session_id;
    context.project_root = deps.project_root;
    // ToolContext::history for every tool this session runs; null when there
    // is no SQLite-backed reader behind it.
    context.history = deps.history;

    // The domain channels block (background task bookkeeping, injections)
    // still needs sharing with sinks that OUTLIVE their invocation --
    // background bash's watcher captures it past the tool call, the turn,
    // and this worker's lifetime. event_sink needs no such care: every copy
    // of it is independently self-sufficient (it is backed by an
    // rpc::EventTranslator, whose own copy of the transport shared_ptr keeps
    // the broadcaster alive on its own), so any sink below may capture it by
    // value regardless of which thread ends up calling it.
    const auto channels = deps.channels;
    const auto event_sink = deps.event_sink;
    const std::string session_id = deps.session_id;

    context.injections = std::shared_ptr<InjectionQueue>(channels, &channels->injections);
    context.session_lifetime = channels;
    // ToolContext::schedule_injection for every tool (the loop copies this onto
    // each context): how a background task hands its result back at a turn
    // boundary. Background bash and python REFUSE to start without it ("no
    // deferred-completion channel"), so leaving it unset made them unavailable in
    // every real session. Captures the channels block, not the Session, because
    // the tool's handle outlives the session; Push refuses after Terminate closes
    // the queue, and Wake reaches the worker only while the session lives.
    context.injection_sink = [channels](llm::Message message) {
        const bool staged = channels->injections.Push(std::move(message));
        if (staged) channels->Wake();
        return staged;
    };

    // The plugin prompt: the loop copies this into every ToolContext (see
    // SessionContext::prompt_user). Null broker leaves it null -- the tool
    // degrades -- rather than wiring a call that would dereference nothing.
    if (deps.prompt_broker != nullptr) {
        tools::PromptBroker& broker = *deps.prompt_broker;
        context.prompt_user = [&broker](const tools::PromptRequest& request) {
            return broker.Prompt(request);
        };
    }

    context.tool_output_sink = [sink = event_sink](std::string_view tool_use_id,
                                                   std::string_view chunk) {
        // The sink never blocks (event_broadcaster.hpp): a slow or absent
        // subscriber drops its oldest and resyncs, and no subscriber at all
        // buffers in the backlog. The turn never waits on a client.
        //
        // ToolOutputDeltaEvent.chunk is a proto3 string: raw process output
        // that is not UTF-8 (a binary `cat`, Latin-1) made the CLIENT's parse
        // of the whole event fail. Scrubbed here, the one door every tool's
        // live output passes; a character split between two reads is held
        // back upstream (AgentLoop's emit_output) so it is not scrubbed.
        sink(SessionEvent{ToolOutputDelta{std::string(tool_use_id), common::ScrubUtf8(chunk)}});
    };

    context.task_event_sink = [channels, sink = event_sink,
                               session_id](const tools::BackgroundTaskEvent& task) {
        auto& live_tasks = channels->background_tasks_live;
        // Quiescence bookkeeping for the reaper (see background_tasks_live).
        // STARTED increments; every terminal state decrements -- floored at
        // zero, so a duplicated terminal event cannot wrap the count downward.
        if (task.state == tools::BackgroundTaskEvent::State::kStarted) {
            live_tasks.fetch_add(1, std::memory_order_acq_rel);
        } else {
            std::size_t current = live_tasks.load(std::memory_order_acquire);
            while (current > 0 &&
                   !live_tasks.compare_exchange_weak(current, current - 1,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            }
        }
        // The caller may be the watcher thread, never the worker. The sink
        // never blocks and refuses only a closed broadcaster (termination):
        // that is the one case worth a warning, because "dropped" events
        // during normal operation are the designed degradation for a stalled
        // subscriber -- the transcript, not this status channel, is the
        // authoritative record.
        if (!sink(SessionEvent{task})) {
            // DefaultLogger is process-lifetime; the session logger is the
            // same object. This lambda can outlive the session's logger
            // reference, but not the logger itself.
            common::DefaultLogger().warn("session {}: background task event for '{}' dropped "
                                         "(session terminated)",
                                         session_id, task.task_id);
        }
    };

    // A compaction rewrites only the working set, never a transcript row, so
    // this event is the client's one live signal that it happened (the same
    // record rides scrollback pages via the compactions table).
    context.compaction_sink = [sink = event_sink](const CompactionEvent& compaction) {
        sink(SessionEvent{compaction});
    };

    context.tool_lifecycle_sink = [sink = event_sink, observers = &deps.observers,
                                   info_snapshot = deps.info_snapshot,
                                   hook_labels = deps.hook_labels](
                                      const ToolLifecycleEvent& lifecycle) {
        sink(SessionEvent{lifecycle});

        // AND THE OBSERVER SIDE, which had no producer at all. ARCHITECTURE.md
        // documents OnToolExecuted as one of the four SessionObserver hooks, and
        // NotifyToolExecuted's only caller in the whole tree was its own unit
        // test -- so a plugin implementing the documented hook got silence,
        // which is worse than the hook not existing.
        //
        // On the FINISH event only: a started/finished pair is the wire's
        // concern (a client has a panel to clear), whereas an observer wants one
        // record of a completed call. Fired for a FAILED tool too, for the same
        // reason the wire event is.
        if (lifecycle.started) return;
        ToolInvocation invocation;
        invocation.tool_use_id = lifecycle.tool_use_id;
        invocation.tool_name = lifecycle.tool_name;
        invocation.is_error = lifecycle.is_error;
        invocation.duration = lifecycle.duration;
        auto labels = hook_labels(lifecycle.tool_name, lifecycle.input);
        invocation.permission_key = std::move(labels.permission_key);
        invocation.category = std::move(labels.category);
        // `summary` stays empty deliberately: the only honest summary is the
        // tool's own disclosure, which is built for a permission prompt and is
        // not reached when the call is pre-approved. An observer that wants
        // detail has SessionReader and the tool_use_id.
        observers->NotifyToolExecuted(info_snapshot(), std::move(invocation));
    };

    // Returning false aborts the provider request. This is the cancellation path
    // that termination reuses -- there is no second mechanism, and adding one is
    // how a session ends up with two half-working ways to stop.
    //
    // The sink's cancel check is the cancel half (a full queue can no longer
    // refuse anything -- the sink is drop-oldest, never blocking). The refusal
    // false is now purely Terminate: closed means the session is ending, and the
    // request must stop.
    out.stream_event_sink = [is_cancelled = deps.is_cancelled,
                             sink = event_sink](const llm::StreamEvent& stream) {
        if (is_cancelled()) return false;
        return sink(SessionEvent{stream});
    };

    // The real-time steering gate. Runs on the WORKER (the loop's flush is
    // worker-only), which is what makes the hook subprocess safe here -- the
    // same thread contract as the turn-pop veto in RunOneTurn. A user-text
    // injection that the core.user_prompt_submit hook blocks is dropped
    // before it is persisted, with the reason on the wire -- identical
    // observable semantics to a vetoed turn-pop. A subagent never steers (its
    // seeded task is not a user prompt; its sessions receive no user turns),
    // mirroring the turn-pop seam's top-level-only rule.
    context.steer_gate = [hooks = deps.hooks, parent_empty = deps.parent_session_id.empty(),
                          session_id, project_root = deps.project_root,
                          is_cancelled = deps.is_cancelled, sink = event_sink,
                          &logger = deps.logger](const llm::Message& message) {
        if (hooks == nullptr || !parent_empty) return true;
        std::string prompt;
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (!prompt.empty()) prompt += "\n";
                prompt += text->text;
            }
        }
        nlohmann::json payload{{"session_id", session_id},
                               {"project_root", project_root.string()},
                               {"prompt", prompt}};
        if (auto blocked = hooks->Emit(hooks::kUserPromptSubmit, std::move(payload),
                                       [&] { return is_cancelled(); })) {
            logger.warn("session {}: user_prompt_submit hook blocked a steered message: {}",
                        session_id, *blocked);
            sink(SessionEvent{SessionError{"steered message rejected by hook: " + *blocked}});
            return false;
        }
        return true;
    };

    return out;
}

}  // namespace minimal::agent
