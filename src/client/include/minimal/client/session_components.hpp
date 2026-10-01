#pragma once

// SessionState's components (state.hpp). Plain classes owned by SessionState
// and guarded by ITS mutex: no locking here, every method runs with the
// owner's mtx_ held. The facade keeps the orchestration of events that span
// components (turn end, page apply, rewind, resync) and the derived tab
// state; each component keeps one concern's data and rules.

#include <optional>
#include <string_view>

#include "minimal/client/session_types.hpp"

namespace minimal::client {

// The three modal requests a session can be parked on. Mutually exclusive:
// setting one drops the others, and every site that ends one ends all.
class PendingPrompts {
public:
    void SetPermission(PendingPermission request);
    void SetConfirmation(PendingConfirmation request);
    void SetPluginPrompt(PendingPluginPrompt request);

    // Clears whichever pending request has `request_id`; true when one did.
    bool ClearFor(std::string_view request_id);
    void ClearAll();
    bool Any() const { return permission_ || confirmation_ || plugin_prompt_; }

    const std::optional<PendingPermission>& permission() const { return permission_; }
    const std::optional<PendingConfirmation>& confirmation() const { return confirmation_; }
    const std::optional<PendingPluginPrompt>& plugin_prompt() const { return plugin_prompt_; }

private:
    std::optional<PendingPermission> permission_;
    std::optional<PendingConfirmation> confirmation_;
    std::optional<PendingPluginPrompt> plugin_prompt_;
};

}  // namespace minimal::client
