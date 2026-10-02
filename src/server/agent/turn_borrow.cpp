#include "minimal/agent/turn_borrow.hpp"

#include <exception>
#include <stdexcept>

namespace minimal::agent {

TurnBorrow::~TurnBorrow() {
    if (!active_) return;
    // Unconditional, provider included, so a borrow can never leak into the next
    // turn. The run mode is read AT RESTORE TIME, not snapshotted at pop: a
    // snapshot races the reader-side switch in both directions (a SetRunMode
    // landing DURING a borrowed turn would be reverted by a pop-time snapshot).
    deps_.apply(deps_.session_preset, *deps_.session_provider, deps_.desired_run_mode());
}

std::optional<std::string> TurnBorrow::Begin(const QueuedTurn* overrides) {
    if (overrides == nullptr || (overrides->model.empty() && overrides->agent_preset.empty())) {
        return std::nullopt;
    }
    try {
        // active before ANY mutation, and kept set on failure: a borrow that
        // throws mid-way (after SetRunMode/SetPreset, during provider
        // construction) must still unwind to the pre-borrow state, not leave a
        // half-applied borrow behind. On a fully-failed borrow the restore is a
        // harmless no-op rebind.
        active_ = true;
        AgentPreset transient = deps_.session_preset;
        if (!overrides->agent_preset.empty()) {
            if (deps_.presets == nullptr) {
                throw std::runtime_error("no preset registry wired; cannot resolve agent '" +
                                         overrides->agent_preset + "'");
            }
            transient = deps_.presets->Resolve(overrides->agent_preset, deps_.project_root);
        }
        // The ref is split here (pop time), so a malformed one fails the turn by
        // name instead of being dropped reader-side.
        std::optional<common::ModelRef> model;
        if (!overrides->model.empty()) {
            model = common::SplitModelRef(overrides->model);
            if (!model) {
                throw std::runtime_error("malformed model '" + overrides->model +
                                         "'; expected \"<provider>/<model>\"");
            }
        } else {
            model = deps_.model_override;
        }
        if (model) {
            if (deps_.provider_resolver == nullptr) {
                throw std::runtime_error("no provider resolver wired; cannot resolve model '" +
                                         model->str() + "'");
            }
            transient = deps_.provider_resolver->ResolveModel(std::move(transient), model);
        }
        // A different instance only when the borrowed preset is served by a
        // different connection; the cache makes a known entry a hit.
        if (!ProviderResolver::SameConnection(transient, deps_.session_preset)) {
            if (deps_.provider_resolver == nullptr) {
                throw std::runtime_error("no provider resolver wired; cannot switch provider "
                                         "for the per-turn override");
            }
            borrowed_provider_ = deps_.provider_resolver->Acquire(transient);
        }
        // The borrowed preset's gates govern the borrowed turn. Its run mode can
        // only narrow the session's CURRENT posture, never the default from
        // params/preset.
        deps_.apply(transient, borrowed_provider_ ? *borrowed_provider_ : *deps_.session_provider,
                    tools::clamp_run_mode(deps_.desired_run_mode(), transient.run_mode));
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
    return std::nullopt;
}

}  // namespace minimal::agent
