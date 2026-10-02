#pragma once

// One tool round: given the tool_use blocks of an assistant message, GATE every
// call (permission decisions, input ceiling, "deny and stop"), then RUN them --
// parallel-safe ones on the pool, the rest serially on the worker -- and hand
// back one result block per call, in block order. Persisting the round (the held
// assistant message plus these results, as one unit) stays with the loop, which
// owns the store and the working set.
//
// Phase 1 (gate) is sequential on the calling worker so prompts arrive one at a
// time and nothing can park in the permission checker on a pool thread -- what
// keeps Session::Terminate's fixed order deadlock-free. Phase 2 (run) executes
// the gated calls; results land by index so the transcript is deterministic
// regardless of completion order.
//
// Worker-thread only. Owns the ToolPool (so a loop-only test exercises the real
// dispatch path, and the pool cannot outlive the executor/registry it borrows --
// the round is a loop member declared after them).

#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/loop_types.hpp"
#include "minimal/agent/request_builder.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/tools/cancel_token.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_pool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

class ToolRound {
public:
    ToolRound(tools::ToolExecutor& executor, tools::ToolRegistry& registry,
              const AgentPreset& preset, const LoopConfig& config, const SessionContext& session,
              RequestBuilder& request, std::function<bool()> is_cancelled, int pool_width)
        : executor_(executor),
          registry_(registry),
          preset_(preset),
          config_(config),
          session_(session),
          request_(request),
          is_cancelled_(std::move(is_cancelled)),
          pool_(pool_width) {}

    // Tokens a tool parked on a condition variable (an MCP call) wakes from on
    // cancel; minted per turn by the Session.
    void SetCancelToken(tools::CancelTokenPtr token) { cancel_token_ = std::move(token); }

    struct Outcome {
        // One block per tool_use, in block order.
        std::vector<llm::ContentBlock> results;
        // "Deny and stop": the operator is done with this turn; no provider call
        // follows the round.
        bool stop_requested = false;
    };

    // Gates and runs `tool_uses` (pointers into the assistant message, which must
    // outlive the call). Appends each call's name to `tool_names_invoked` at gate
    // time, in block order. Throws only for failures the round cannot absorb; a
    // tool's own failure is an error result, never an exception.
    Outcome Run(const std::vector<const llm::ToolUseBlock*>& tool_uses,
                std::vector<std::string>& tool_names_invoked);

private:
    bool Cancelled() const { return is_cancelled_ && is_cancelled_(); }

    tools::ToolExecutor& executor_;
    tools::ToolRegistry& registry_;
    const AgentPreset& preset_;
    const LoopConfig& config_;
    const SessionContext& session_;
    RequestBuilder& request_;
    std::function<bool()> is_cancelled_;
    tools::CancelTokenPtr cancel_token_;
    // Width from LoopConfig; width 1 never starts threads.
    tools::ToolPool pool_;
};

}  // namespace minimal::agent
