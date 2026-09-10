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
    explicit HostAttentionSink(AttentionState* state) : state_(state) {}

    void RaiseTab(const std::string& session_id) override {
        if (state_ != nullptr) state_->RaiseTab(session_id);
    }
    void ClearTab(const std::string& session_id) override {
        if (state_ != nullptr) state_->ClearTab(session_id);
    }
    void RaiseGlobal() override {
        if (state_ != nullptr) state_->RaiseGlobal();
    }
    void ClearGlobal() override {
        if (state_ != nullptr) state_->ClearGlobal();
    }

private:
    AttentionState* state_;  // host-owned; may be null when the seam is not offered
};

// --- HostClientApi --------------------------------------------------------------

class HostClientApi final : public ClientApi {
public:
    explicit HostClientApi(SessionClient* client) : client_(client) {}

    std::optional<MessagePage> GetMessages(const std::string& session_id,
                                           std::optional<std::int64_t> before_seq,
                                           int limit) override {
        if (client_ == nullptr) return std::nullopt;

        v1::GetMessagesResponse response;
        grpc::ClientContext context;
        // Bounded, like core's own scrollback refresh: a wedged server costs
        // the caller one failed page, never a hung thread.
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        v1::GetMessagesRequest request;
        request.set_session_id(session_id);
        if (before_seq.has_value()) request.set_before_seq(*before_seq);
        request.set_limit(limit);
        if (!client_->GetMessages(&context, request, &response).ok()) return std::nullopt;

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

void HostFocusProvider::Attach(AppState* app) { app_ = app; }

std::string HostFocusProvider::FocusedSessionId() const {
    // Refuses until Attach (see the header): "" is the interface's own
    // "no focus knowledge" answer, not a distinct condition.
    if (app_ == nullptr) return {};
    return app_->FocusedSessionId();
}

std::vector<TabInfo> HostFocusProvider::Tabs() const {
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

std::unique_ptr<AttentionSink> MakeHostAttentionSink(AttentionState* state) {
    return std::make_unique<HostAttentionSink>(state);
}

std::unique_ptr<ClientApi> MakeHostClientApi(SessionClient* client) {
    return std::make_unique<HostClientApi>(client);
}

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider() {
    return std::make_unique<HostFocusProvider>();
}

}  // namespace minimal::client::plugins
