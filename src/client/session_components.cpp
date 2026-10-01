#include "minimal/client/session_components.hpp"

#include <utility>

namespace minimal::client {

// --- PendingPrompts ------------------------------------------------------------

void PendingPrompts::SetPermission(PendingPermission request) {
    ClearAll();
    permission_ = std::move(request);
}

void PendingPrompts::SetConfirmation(PendingConfirmation request) {
    ClearAll();
    confirmation_ = std::move(request);
}

void PendingPrompts::SetPluginPrompt(PendingPluginPrompt request) {
    ClearAll();
    plugin_prompt_ = std::move(request);
}

bool PendingPrompts::ClearFor(std::string_view request_id) {
    bool cleared = false;
    if (permission_ && permission_->request_id == request_id) {
        permission_.reset();
        cleared = true;
    }
    if (confirmation_ && confirmation_->request_id == request_id) {
        confirmation_.reset();
        cleared = true;
    }
    if (plugin_prompt_ && plugin_prompt_->request_id == request_id) {
        plugin_prompt_.reset();
        cleared = true;
    }
    return cleared;
}

void PendingPrompts::ClearAll() {
    permission_.reset();
    confirmation_.reset();
    plugin_prompt_.reset();
}

}  // namespace minimal::client
