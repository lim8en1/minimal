#include "minimal/agent/tool_round.hpp"

#include <chrono>
#include <memory>
#include <mutex>

#include "minimal/common/utf8.hpp"
#include "tool_result_caps.hpp"

namespace minimal::agent {

ToolRound::Outcome ToolRound::Run(const std::vector<const llm::ToolUseBlock*>& tool_uses,
                                  std::vector<std::string>& tool_names_invoked) {
    // Parallel dispatch (ARCHITECTURE.md "Tool dispatch"), in two phases.
    //
    // Phase 1 -- GATE. Every call's permission decision resolves here,
    // sequentially on this worker, in assistant-block order. Prompts still
    // arrive one at a time (better UX than five simultaneous modals); a
    // grant an earlier call of the round acquires satisfies later ones; and
    // nothing below can park in the permission checker on a pool thread,
    // which is what keeps Session::Terminate's fixed order deadlock-free.
    // Prepare never throws: a gate refusal or a plugin throw becomes the
    // denial result Execute returns.
    struct PendingCall {
        std::size_t index;  // position in assistant-block order
        tools::PreparedInvocation prepared;
        tools::ToolContext ctx;
        bool to_pool = false;
    };
    std::vector<PendingCall> calls;
    calls.reserve(tool_uses.size());
    // "Deny and stop" (PreparedInvocation::stop_turn): once any call of the
    // round is denied that way, the operator is done with this turn -- the
    // later calls are answered without prompting, the earlier (already
    // allowed) ones without running, and no provider call follows. Nothing
    // has executed yet when the flag lands: gating precedes all execution.
    bool stop_requested = false;
    const auto stopped_by_user = [](const std::string& tool_use_id) {
        return llm::ToolResultBlock{
            tool_use_id, {llm::TextBlock{"not run: the user stopped the turn"}}, true};
    };
    for (std::size_t i = 0; i < tool_uses.size(); ++i) {
        const auto* use = tool_uses[i];
        // Aggregate-built around its context: ToolContext holds a Logger
        // reference, so it (and therefore PendingCall) is construct-in-place
        // only -- no default construction, no assignment.
        PendingCall call{i,
                         {},
                         [&] {
                             tools::ToolContext ctx{session_.logger};
                              ctx.session_id = session_.session_id;
                              ctx.project_root = session_.project_root;
                              ctx.is_cancelled = is_cancelled_;
                             ctx.cancel_token = cancel_token_;
                             // Per invocation, from the Session-owned window: a
                             // subagent sees only its own transcript, because the
                             // window is bound to THIS session's id
                             // (docs/EXTENSIBILITY.md §2.1).
                             ctx.history = session_.history;
                              if (session_.tool_output_sink) {
                                  const std::string tool_use_id = use->id;
                                  // A process's reads split anywhere -- also inside
                                  // a multi-byte character. The incomplete tail is
                                  // held back and prepended to the next chunk, so
                                  // the sink's UTF-8 scrub (session.cpp) does not
                                  // turn it into U+FFFD. Locked: stdout and stderr
                                  // readers may emit concurrently.
                                  struct Carry {
                                      std::mutex mu;
                                      std::string pending;
                                  };
                                  // Captures the sink by value, never `this`:
                                  // a background task keeps emit_output past
                                  // the round, the loop and the Session.
                                  ctx.emit_output = [sink = session_.tool_output_sink,
                                                     tool_use_id,
                                                     carry = std::make_shared<Carry>()](
                                                        std::string_view chunk) {
                                      std::string text;
                                      {
                                          std::lock_guard lock(carry->mu);
                                          text = std::move(carry->pending);
                                          carry->pending.clear();
                                          text.append(chunk);
                                          const std::size_t tail =
                                              common::IncompleteUtf8Tail(text);
                                          carry->pending.assign(text, text.size() - tail,
                                                                tail);
                                          text.resize(text.size() - tail);
                                      }
                                      if (!text.empty()) sink(tool_use_id, text);
                                  };
                              }
                              // Deferred-completion surface, copied verbatim from the session's
                              // wiring (see SessionContext): a tool that schedules work beyond
                              // its own invocation captures these, and their lifetime is the
                              // session's shared channels block -- not this call, not this loop.
                              ctx.schedule_injection = session_.injection_sink;
                              ctx.session_lifetime = session_.session_lifetime;
                              // Stamped with the invoking tool's surface name, so
                              // a client labels a background row without knowing
                              // which tools run background work.
                              if (session_.task_event_sink) {
                                  ctx.emit_task_event =
                                      [sink = session_.task_event_sink, tool_name = use->name](
                                          const tools::BackgroundTaskEvent& event) {
                                          tools::BackgroundTaskEvent stamped = event;
                                          stamped.tool_name = tool_name;
                                          sink(stamped);
                                      };
                              }
                              ctx.prompt_user = session_.prompt_user;
                              return ctx;
                          }(),
                          false};

        // Recorded at gate time, in block order: what ran is the question,
        // not what succeeded, and call order is the model's order -- not
        // the order executions happen to finish in.
        tool_names_invoked.push_back(use->name);

        // THE GLOBAL TOOL-INPUT CEILING, checked before anything else
        // consumes the input: no hook payload, no permission request, no
        // execution -- one error result the model adapts to, the same
        // shape as every other refusal. (The lifecycle events still fire
        // with the input, so a client panel shows WHAT was refused.)
        tools::PreparedInvocation prepared;
        prepared.tool_use_id = use->id;
        prepared.tool_name = use->name;
        bool denied_for_input = false;
        // A cancel landing during an earlier call's prompt must also stop
        // the remaining gates: otherwise each one raises a fresh prompt
        // (the epoch bump only released the waiter that was parked).
        if (!stop_requested && Cancelled()) stop_requested = true;
        if (stop_requested) {
            // Not prompted, not run: see stop_requested above. Takes the
            // same skip-Prepare path as an oversized input.
            prepared.denial = stopped_by_user(use->id);
            denied_for_input = true;
        } else if (const auto reply = config_.strategy_tool_replies.find(use->name);
                   reply != config_.strategy_tool_replies.end()) {
            // A compaction strategy's tool, declared on every request for its
            // summarizer's sake (see BuildSystemAndTools); outside a
            // compaction it does nothing.
            prepared.denial = llm::ToolResultBlock{use->id, {llm::TextBlock{reply->second}}, true};
            denied_for_input = true;
        } else if (config_.max_tool_input_bytes > 0) {
            const std::size_t input_bytes = use->input.dump().size();
            if (input_bytes > static_cast<std::size_t>(config_.max_tool_input_bytes)) {
                prepared.denial = llm::ToolResultBlock{
                    use->id,
                    {llm::TextBlock{"tool input exceeds the configured max_input_bytes (" +
                                    std::to_string(config_.max_tool_input_bytes) +
                                    " bytes); send a smaller input"}},
                    true};
                denied_for_input = true;
            }
        }
        if (!denied_for_input) {
            // Per-preset plugin config, resolved BEFORE Prepare: Disclose()
            // and grant checks run inside it, so the disclosure (Ask prompt,
            // grant keys) must already see the slice the tool's
            // config_namespace() claims in the active preset's tool_config.
            // Points into preset_, which is stable for the turn -- preset
            // swaps land on this worker between turns (Session::ApplyPreset),
            // never inside one. The Find is the same lookup PrepareGuarded
            // performs; AliasedTool forwards config_namespace().
            if (const auto tool = registry_.Find(use->name)) {
                const std::string_view ns = tool->config_namespace();
                if (!ns.empty()) {
                    if (auto it = preset_.tool_config.find(std::string(ns));
                        it != preset_.tool_config.end()) {
                        call.ctx.tool_config = &it.value();
                    }
                }
            }
            prepared = executor_.Prepare(use->id, use->name, use->input, call.ctx);
            if (prepared.stop_turn) stop_requested = true;
        }
        // Unknown tools arrive here with no resolved tool; they execute as
        // their denial result and have no exclusivity to protect.
        call.to_pool = !denied_for_input && prepared.tool &&
                       prepared.tool->parallel_safe();
        call.prepared = std::move(prepared);
        calls.push_back(std::move(call));
    }

    // Calls gated BEFORE the stopping one were allowed but have not run:
    // stop means stop, so they are answered unrun too. Every tool_use still
    // gets its result, which keeps the transcript paired.
    if (stop_requested) {
        for (auto& call : calls) {
            if (call.prepared.denial) continue;
            call.prepared.denial = stopped_by_user(tool_uses[call.index]->id);
            call.to_pool = false;
        }
    }

    // Phase 2 -- RUN. The brackets below run on whichever thread executes
    // the call: both sinks are thread-safe (the lifecycle/output sinks end
    // in the session's EventQueue, already multi-producer; the observer
    // registry dispatches internally on its one ordered thread). Results
    // land by index, so the appended User message is in block order
    // regardless of completion order -- a deterministic transcript and a
    // stable prompt-cache prefix.
    std::vector<llm::ContentBlock> results;
    results.resize(tool_uses.size());
    // Wire-event failure must never take the tool result down: the sinks
    // end in the session's EventQueue, the transcript is the authoritative
    // record, and a throw here would otherwise unwind through a pool task.
    auto emit_lifecycle = [this](const ToolLifecycleEvent& event) {
        try {
            if (session_.tool_lifecycle_sink) session_.tool_lifecycle_sink(event);
        } catch (const std::exception& error) {
            session_.logger.warn("tool lifecycle event dropped: {}", error.what());
        } catch (...) {
            session_.logger.warn("tool lifecycle event dropped (unknown exception)");
        }
    };
    auto execute_one = [&](PendingCall& call) {
        const auto* use = tool_uses[call.index];
        ToolLifecycleEvent lifecycle;
        lifecycle.tool_use_id = use->id;
        lifecycle.tool_name = use->name;
        lifecycle.input = use->input;
        if (call.prepared.tool) {
            try {
                lifecycle.summary = call.prepared.tool->Summary(use->input);
            } catch (...) {
                // A display nicety: a throwing Summary just leaves it empty.
            }
        }
        lifecycle.started = true;
        lifecycle.started_at_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        emit_lifecycle(lifecycle);

        const auto tool_started = std::chrono::steady_clock::now();
        llm::ToolResultBlock tool_result;
        try {
            struct InToolScope {
                InToolScope() { g_in_tool_execution = true; }
                ~InToolScope() { g_in_tool_execution = false; }
            } in_tool;
            tool_result = executor_.Execute(call.prepared, call.ctx);
        } catch (const std::exception& error) {
            // Execute never throws by contract (tool_executor.hpp) -- but
            // this is the one seam whose failure mode is std::terminate
            // THROUGH a pool worker, and an unwind here would also race
            // the round's other submitted tasks against this stack frame.
            // An error result keeps every guarantee instead: the transcript
            // stays paired, the round appends, the model adapts.
            session_.logger.warn("tool {} threw past its executor guard: {}", use->name,
                                 error.what());
            tool_result = llm::ToolResultBlock{
                use->id, {llm::TextBlock{"tool execution failed unexpectedly"}}, true};
        } catch (...) {
            session_.logger.warn("tool {} threw past its executor guard (unknown exception)",
                                 use->name);
            tool_result = llm::ToolResultBlock{
                use->id, {llm::TextBlock{"tool execution failed unexpectedly"}}, true};
        }

        // THE GLOBAL TOOL-RESULT CEILING (see CapToolResult): applies
        // regardless of which tool produced this, including MCP tools and
        // any third-party plugin tool that caps nothing on its own.
        CapToolResult(tool_result, request_.ToolResultByteCap());
        if (const std::size_t dropped =
                CapToolDisplay(tool_result, config_.max_tool_display_bytes)) {
            session_.logger.warn(
                "tool {}: display payload ({} bytes) exceeds tools.max_display_bytes ({}); "
                "dropped, the client falls back to text",
                use->name, dropped, config_.max_tool_display_bytes);
        }

        // Emitted even when Execute() reports an error, and before the
        // result is appended: a client that showed a tool as started must
        // always see it finish, or its running-tools panel accumulates
        // entries that never clear. The input rode the STARTED event; the
        // finish event carries the outcome, not another full copy of the
        // input -- for a Write with a 1 MB payload that was a second
        // megabyte into the (now byte-bounded) outbound queue per call,
        // for data every consumer already had.
        lifecycle.started = false;
        lifecycle.input = {};
        lifecycle.is_error = tool_result.is_error;
        lifecycle.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tool_started);
        emit_lifecycle(lifecycle);

        results[call.index] = std::move(tool_result);
    };

    // Parallel-safe calls go to the pool first, so the serial ones below
    // overlap as much of the batch as possible. Serial-only tools
    // (parallel_safe() == false) run here on the worker, one at a time, in
    // block order: their contract is "my invocations never overlap each
    // other", and the worker is the thread that guarantees it.
    //
    // RAII, not a bare call after both loops: pool tasks capture `&call`
    // into `calls`, which lives on THIS stack frame (ToolPool rule 3 --
    // the caller's stack must outlive Wait()). If anything between the
    // submit loop and the explicit Wait() below throws -- a serial
    // execute_one(), or Submit() itself hitting the pool's own
    // exceptions -- the stack would unwind past `calls` without ever
    // joining the pool, leaving its threads holding dangling references.
    // Wait() is documented idempotent, so the guard's destructor and the
    // explicit call below (kept for the ordinary, non-throwing path's
    // readability) cost nothing extra when both run.
    struct PoolWaitGuard {
        tools::ToolPool& pool;
        ~PoolWaitGuard() { pool.Wait(); }
    } wait_guard{pool_};

    for (auto& call : calls) {
        if (call.to_pool) {
            // `call` lives in `calls` on this stack until Wait() returns,
            // which is why a reference capture is sound (ToolPool rule 3).
            pool_.Submit([&call, &execute_one] { execute_one(call); });
        }
    }
    for (auto& call : calls) {
        if (!call.to_pool) execute_one(call);
    }
    pool_.Wait();

    return Outcome{std::move(results), stop_requested};
}

}  // namespace minimal::agent
