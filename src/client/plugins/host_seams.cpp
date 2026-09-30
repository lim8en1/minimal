#include "minimal/client/plugins/host_seams.hpp"

#include <chrono>
#include <utility>

#include <grpcpp/client_context.h>

#include "minimal/client/state.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {
namespace {

// --- HostAttentionSink --------------------------------------------------------

class HostAttentionSink final : public AttentionSink {
public:
    HostAttentionSink(AttentionState* state, HostUi* ui) : state_(state), ui_(ui) {}

    void RaiseTab(const std::string& session_id) override {
        if (state_ != nullptr) state_->RaiseTab(session_id);
        Redraw();
    }
    void ClearTab(const std::string& session_id) override {
        if (state_ != nullptr) state_->ClearTab(session_id);
        Redraw();
    }
    void RaiseGlobal() override {
        if (state_ != nullptr) state_->RaiseGlobal();
        Redraw();
    }
    void ClearGlobal() override {
        if (state_ != nullptr) state_->ClearGlobal();
        Redraw();
    }

private:
    void Redraw() {
        if (ui_ != nullptr) ui_->RequestRedraw();
    }
    AttentionState* state_;  // host-owned; may be null when the seam is not offered
    HostUi* ui_;             // likewise; null: no repaint request
};

// --- HostClientApi --------------------------------------------------------------

class HostClientApi final : public ClientApi {
public:
    explicit HostClientApi(SessionClient* client) : client_(client) {}

    std::optional<MessagePage> GetMessages(const std::string& session_id,
                                           std::optional<std::int64_t> before_seq,
                                           int limit) override {
        if (client_ == nullptr) return std::nullopt;

        // Bounded, like core's own scrollback refresh: a wedged server costs
        // the caller one failed page, never a hung thread.
        const auto result = client_->GetMessages(session_id, before_seq, limit);
        if (!result.status.ok()) return std::nullopt;
        const v1::GetMessagesResponse& response = result.response;

        MessagePage page;
        for (const auto& stored : response.messages()) {
            page.messages.push_back(stored);
        }
        page.has_more = response.has_more();
        return page;
    }

    std::optional<std::string> PluginCall(const std::string& plugin_name,
                                          const std::string& method,
                                          const std::string& json_payload) override {
        if (client_ == nullptr) return std::nullopt;
        const auto result = client_->PluginCall(plugin_name, method, json_payload);
        if (!result.status.ok()) return std::nullopt;
        return result.json_payload;
    }

    SubscribeOutcome PluginSubscribe(const std::string& plugin_name, const std::string& method,
                                     const std::string& json_payload,
                                     const std::function<bool(const std::string&)>& on_payload) override {
        if (client_ == nullptr) return SubscribeOutcome::kError;
        // Blocking on the CALLING thread -- a plugin subscribing is blocking
        // its own thread by choice; the host never subscribes for it.
        grpc::Status status;
        const auto outcome = client_->PluginSubscribe(plugin_name, method, json_payload,
                                                      on_payload, &status);
        if (!status.ok()) {
            minimal::common::DefaultLogger().warn("plugin subscription {}/{} ended in error: {}",
                                         plugin_name, method, status.error_message());
        }
        return outcome;
    }

private:
    SessionClient* client_;  // host-owned; the one channel, never a plugin's
};

}  // namespace

// --- HostFocusProvider ----------------------------------------------------------

void HostFocusProvider::Attach(AppState* app) {
    std::lock_guard lk(mtx_);
    app_ = app;
}

void HostFocusProvider::Detach() {
    std::lock_guard lk(mtx_);
    app_ = nullptr;
}

std::string HostFocusProvider::FocusedSessionId() const {
    // Refuses until Attach, or once Detach has run (see the header): "" is
    // the interface's own "no focus knowledge" answer, not a distinct
    // condition. Held for the whole call: see Detach()'s comment for why
    // this is what makes it a real barrier, not just a racy null-out.
    std::lock_guard lk(mtx_);
    if (app_ == nullptr) return {};
    return app_->FocusedSessionId();
}

std::vector<TabInfo> HostFocusProvider::Tabs() const {
    std::lock_guard lk(mtx_);
    if (app_ == nullptr) return {};
    // LIGHT snapshots only (include_tool_output=false): a focus poller needs
    // identity and names, never tool buffers -- the same reason the tools
    // panel reads the light table (state.hpp track R).
    std::vector<TabInfo> out;
    for (const auto& tab : app_->GetAllTabs(/*include_tool_output=*/false)) {
        TabInfo info;
        info.tab_id = tab.tab_id;
        info.session_id = tab.session_id;
        info.display_name = tab.display_name;
        if (tab.is_focused) {
            out.insert(out.begin(), std::move(info));
        } else {
            out.push_back(std::move(info));
        }
    }
    return out;
}

void HostFocusProvider::RequestRedraw() {
    std::lock_guard lk(mtx_);
    if (app_ != nullptr) app_->NotifyExternalUpdate();   // an atomic bump; never blocks
}

void HostFocusProvider::ShowView(const std::string& view_id) {
    std::lock_guard lk(mtx_);
    if (app_ != nullptr) app_->RequestView(view_id);
}

std::unique_ptr<AttentionSink> MakeHostAttentionSink(AttentionState* state, HostUi* ui) {
    return std::make_unique<HostAttentionSink>(state, ui);
}

std::unique_ptr<ClientApi> MakeHostClientApi(SessionClient* client) {
    return std::make_unique<HostClientApi>(client);
}

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider() {
    return std::make_unique<HostFocusProvider>();
}

}  // namespace minimal::client::plugins
