#include "minimal/client/plugins/host_seams.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <grpcpp/client_context.h>

#include "minimal/client/command_line.hpp"
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

    std::optional<v1::ListModelsResponse> ListModels(bool refresh_discovery,
                                                     std::string* error) override {
        if (client_ == nullptr) {
            if (error != nullptr) *error = "not connected to a server";
            return std::nullopt;
        }
        auto result = client_->ListModels({.refresh_discovery = refresh_discovery});
        if (!result.status.ok()) {
            if (error != nullptr) *error = result.status.error_message();
            return std::nullopt;
        }
        return std::move(result.response);
    }

    std::optional<v1::ListAgentPresetsResponse> ListAgentPresets(std::string* error) override {
        if (client_ == nullptr) {
            if (error != nullptr) *error = "not connected to a server";
            return std::nullopt;
        }
        // The client's cwd: the same project root session creation sends.
        auto result = client_->ListAgentPresets(CurrentProjectRootOrEmpty());
        if (!result.status.ok()) {
            if (error != nullptr) *error = result.status.error_message();
            return std::nullopt;
        }
        return std::move(result.response);
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

void HostFocusProvider::AttachReads(UiReads reads) { reads_ = std::move(reads); }

std::vector<KeyBindingInfo> HostFocusProvider::KeyBindings() const {
    return reads_.key_bindings ? reads_.key_bindings() : std::vector<KeyBindingInfo>{};
}

std::vector<CommandCatalogEntry> HostFocusProvider::CommandCatalog() const {
    return reads_.command_catalog ? reads_.command_catalog() : std::vector<CommandCatalogEntry>{};
}

std::vector<std::string> HostFocusProvider::ThemeNames() const {
    return reads_.theme_names ? reads_.theme_names() : std::vector<std::string>{};
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

void HostFocusProvider::Enqueue(ViewRequest request) {
    {
        std::lock_guard lk(requests_mtx_);
        requests_.push_back(std::move(request));
    }
    RequestRedraw();   // the frame that applies it
}

void HostFocusProvider::ShowView(const std::string& view_id) {
    Enqueue({ViewRequest::Op::kShow, view_id});
}

void HostFocusProvider::CloseView(const std::string& view_id) {
    Enqueue({ViewRequest::Op::kClose, view_id});
}

void HostFocusProvider::SetPanelVisible(const std::string& view_id, bool visible) {
    Enqueue({ViewRequest::Op::kSetPanelVisible, view_id, visible});
}

void HostFocusProvider::FocusPanel(const std::string& view_id) {
    Enqueue({ViewRequest::Op::kFocusPanel, view_id});
}

class HostFocusProvider::BoundUi final : public HostUi {
public:
    BoundUi(HostFocusProvider& host, std::string plugin) : host_(host), plugin_(std::move(plugin)) {}
    void RequestRedraw() override { host_.RequestRedraw(); }
    void ShowView(const std::string& view_id) override {
        host_.Enqueue({ViewRequest::Op::kShow, view_id, true, plugin_});
    }
    void CloseView(const std::string& view_id) override {
        host_.Enqueue({ViewRequest::Op::kClose, view_id, true, plugin_});
    }
    void SetPanelVisible(const std::string& view_id, bool visible) override {
        host_.Enqueue({ViewRequest::Op::kSetPanelVisible, view_id, visible, plugin_});
    }
    void FocusPanel(const std::string& view_id) override {
        host_.Enqueue({ViewRequest::Op::kFocusPanel, view_id, true, plugin_});
    }
    std::vector<KeyBindingInfo> KeyBindings() const override { return host_.KeyBindings(); }
    std::vector<CommandCatalogEntry> CommandCatalog() const override {
        return host_.CommandCatalog();
    }
    std::vector<std::string> ThemeNames() const override { return host_.ThemeNames(); }

private:
    HostFocusProvider& host_;
    std::string plugin_;
};

HostFocusProvider::HostFocusProvider() = default;
HostFocusProvider::~HostFocusProvider() = default;

HostUi* HostFocusProvider::UiFor(const std::string& plugin_name) {
    std::lock_guard lk(bound_mtx_);
    auto& slot = bound_[plugin_name];
    if (slot == nullptr) slot = std::make_unique<BoundUi>(*this, plugin_name);
    return slot.get();
}

std::vector<ViewRequest> HostFocusProvider::TakeViewRequests() {
    std::lock_guard lk(requests_mtx_);
    std::vector<ViewRequest> out;
    out.swap(requests_);
    return out;
}

std::unique_ptr<AttentionSink> MakeHostAttentionSink(AttentionState* state, HostUi* ui) {
    return std::make_unique<HostAttentionSink>(state, ui);
}

std::unique_ptr<ClientApi> MakeHostClientApi(SessionClient* client) {
    return std::make_unique<HostClientApi>(client);
}

namespace {

class HostWidgetKit final : public WidgetKit {
public:
    explicit HostWidgetKit(const WidgetKitRegistry& registry) : registry_(registry) {}

    ftxui::Component FilterableMenuModal(
        std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
        const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks,
        const Theme& theme) const override {
        return Kit().FilterableMenuModal(std::move(build_rows), spec, std::move(callbacks), theme);
    }
    ftxui::Component SchemaForm(std::function<SchemaFormState()> source,
                                SchemaFormCallbacks callbacks, const Theme& theme) const override {
        return Kit().SchemaForm(std::move(source), std::move(callbacks), theme);
    }
    ftxui::Element ModalFrame(const Theme& theme, const std::string& title,
                              ftxui::Elements rows, ftxui::Elements pinned,
                              const std::string& footer) const override {
        return Kit().ModalFrame(theme, title, std::move(rows), std::move(pinned), footer);
    }
    ftxui::Element ModalRule(const Theme& theme) const override { return Kit().ModalRule(theme); }
    ftxui::Component ModelSelector(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                                   const Theme& theme) const override {
        return Kit().ModelSelector(std::move(spec), std::move(callbacks), theme);
    }

private:
    // Throws while the slot is empty -- only during registration, where the
    // plugin's own guard contains it.
    const WidgetKit& Kit() const {
        const WidgetKit* kit = registry_.get();
        if (kit == nullptr) throw std::logic_error("the widget kit is not available during registration");
        return *kit;
    }

    const WidgetKitRegistry& registry_;
};

}  // namespace

std::unique_ptr<WidgetKit> MakeHostWidgetKit(const WidgetKitRegistry& registry) {
    return std::make_unique<HostWidgetKit>(registry);
}

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider() {
    return std::make_unique<HostFocusProvider>();
}

void HostActionQueue::InsertPrompt(const std::string& text) {
    Enqueue([text](HostActions& host) { host.InsertPrompt(text); });
}

void HostActionQueue::SetTheme(const std::string& name) {
    Enqueue([name](HostActions& host) { host.SetTheme(name); });
}

void HostActionQueue::ChooseAgent(const std::string& name) {
    Enqueue([name](HostActions& host) { host.ChooseAgent(name); });
}

void HostActionQueue::ChooseModel(const std::string& ref) {
    Enqueue([ref](HostActions& host) { host.ChooseModel(ref); });
}

void HostActionQueue::ChooseDefaultModel(const std::string& default_ref) {
    Enqueue([default_ref](HostActions& host) { host.ChooseDefaultModel(default_ref); });
}

void HostActionQueue::SetSubagentModel(const std::string& ref) {
    Enqueue([ref](HostActions& host) { host.SetSubagentModel(ref); });
}

void HostActionQueue::Enqueue(std::function<void(HostActions&)> action) {
    {
        std::lock_guard lk(mtx_);
        queued_.push_back(std::move(action));
    }
    if (ui_ != nullptr) ui_->RequestRedraw();  // the frame that applies it
}

std::vector<std::function<void(HostActions&)>> HostActionQueue::Take() {
    std::lock_guard lk(mtx_);
    std::vector<std::function<void(HostActions&)>> out;
    out.swap(queued_);
    return out;
}

}  // namespace minimal::client::plugins
