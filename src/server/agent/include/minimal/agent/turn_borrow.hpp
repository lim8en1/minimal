#pragma once

// A per-turn model/agent override, applied at turn pop and undone on every exit
// path. The turn BORROWS a preset/provider/run mode; the session KEEPS its
// sticky choices (preset, model_override, metadata stream).
//
// Worker thread, at a turn boundary: the loop is provably not inside a provider
// call, which is the same invariant that makes /model and /agent safe.
//
// Precedence: the turn's explicit model wins over the session's pinned
// model_override, which wins over the (possibly overridden) preset's own model.
// The borrowed preset's run mode can only NARROW the session's current posture
// (clamp_run_mode) -- a per-turn switch is a borrow, never an escalation.
//
// Restore is the destructor, so EVERY exit path -- LlmError, cancellation, a
// non-standard exception out of the turn -- puts back exactly what the session
// holds; a leaked borrow would silently run every later turn on the wrong
// model.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/provider_resolver.hpp"
#include "minimal/agent/work_queue.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::agent {

struct TurnBorrowDeps {
    // The session's own (sticky) state; both must outlive the borrow.
    const AgentPreset& session_preset;
    const std::shared_ptr<llm::LlmProvider>& session_provider;
    const AgentPresetRegistry* presets = nullptr;
    ProviderResolver* provider_resolver = nullptr;
    std::filesystem::path project_root;
    std::optional<common::ModelRef> model_override;  // the session's pinned choice
    // The user's CURRENT run-mode choice, read at call time (the reader can
    // change it mid-turn, and the user's latest word outranks the borrow).
    std::function<tools::PermissionRunMode()> desired_run_mode;
    // Session::ApplyEffective: the one place loop and executor learn the
    // preset/provider governing the next turn.
    std::function<void(const AgentPreset&, llm::LlmProvider&,
                       std::optional<tools::PermissionRunMode>)>
        apply;
};

class TurnBorrow {
public:
    explicit TurnBorrow(TurnBorrowDeps deps) : deps_(std::move(deps)) {}
    TurnBorrow(const TurnBorrow&) = delete;
    TurnBorrow& operator=(const TurnBorrow&) = delete;
    ~TurnBorrow();

    // Applies the overrides, if any. nullopt on success (including "nothing to
    // borrow"); otherwise the reason it failed -- the caller fails the TURN with
    // a named error, nothing persisted. A failed borrow has already been rolled
    // back by the time the destructor runs (it stays armed for a half-applied
    // one).
    std::optional<std::string> Begin(const QueuedTurn* overrides);

private:
    TurnBorrowDeps deps_;
    bool active_ = false;
    // Holds the borrowed provider for the borrow's lifetime: the loop stores a
    // reference to it, which must stay valid through the turn and past the
    // restore's rebind back to the session's provider.
    std::shared_ptr<llm::LlmProvider> borrowed_provider_;
};

}  // namespace minimal::agent
