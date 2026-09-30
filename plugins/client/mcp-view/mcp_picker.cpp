#include "mcp_picker.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/client/view/filterable_menu.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::mcp_view {

namespace {

constexpr const char* kServerPlugin = "mcp";

// Holds the current menu, rebuilt in place whenever the feed has a newer
// snapshot. Render-time rebuild keeps the worker off the UI's objects: the
// worker only publishes data, this component (UI thread) turns it into rows.
class McpPickerComponent final : public ftxui::ComponentBase {
public:
    McpPickerComponent(std::shared_ptr<McpFeed> feed, const client::Theme* theme,
                       plugins::HostUi* ui)
        : feed_(std::move(feed)), theme_(theme), ui_(ui) {}

    ftxui::Element OnRender() override {
        RebuildIfStale();
        return menu_->Render();
    }

    bool OnEvent(ftxui::Event event) override {
        RebuildIfStale();
        return menu_->OnEvent(event);
    }

private:
    void RebuildIfStale() {
        const auto generation = feed_->generation();
        if (menu_ && generation == built_generation_) return;
        built_generation_ = generation;
        auto servers = feed_->snapshot();
        const std::string status = feed_->status();
        auto build_rows = [servers](const std::string& filter) {
            std::vector<client::MenuRow> rows;
            for (const auto& row : BuildMcpPickerRows(servers, filter)) {
                rows.push_back({row.label, row.name, /*header=*/false});
            }
            return rows;
        };
        client::FilterableMenuSpec spec{
            .window_title = "mcp servers",
            .hint_line = " enter: enable/disable  ·  esc: close",
            .status_suffix = [status](const std::string&) { return status; },
            .empty_message = status == "loading" ? " (loading...)" : " (no mcp servers configured)",
        };
        client::FilterableMenuCallbacks callbacks{
            .on_select =
                [feed = feed_](const client::MenuRow& row) {
                    if (!row.header && !row.id.empty()) feed->Toggle(row.id);
                },
            // The host closes plugin modals on Escape before this sees it; a
            // close the menu itself decides on goes through the host seam.
            .on_close =
                [ui = ui_] {
                    if (ui != nullptr) ui->CloseView("mcp");
                },
        };
        if (menu_) DetachAllChildren();
        menu_ = client::MakeFilterableMenuModal(build_rows, spec, callbacks, *theme_);
        Add(menu_);
    }

    std::shared_ptr<McpFeed> feed_;
    const client::Theme* theme_;
    plugins::HostUi* ui_;
    ftxui::Component menu_;
    std::uint64_t built_generation_ = 0;
};

}  // namespace

McpFeed::McpFeed(plugins::ClientApi* api, plugins::HostUi* ui)
    : api_(api), ui_(ui), worker_([this] { Run(); }) {}

McpFeed::~McpFeed() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void McpFeed::Post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void McpFeed::Run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            common::DefaultLogger().warn("mcp-view: {}", e.what());
        }
    }
}

void McpFeed::Refresh() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (servers_.empty()) status_ = "loading";
        ++generation_;
    }
    Post([this] { FetchList(); });
}

void McpFeed::Toggle(const std::string& name) {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& server : servers_) {
            if (server.name == name) enabled = server.enabled;
        }
    }
    Post([this, name, enabled] {
        const auto answer = api_->PluginCall(
            kServerPlugin, "set_enabled",
            nlohmann::json{{"name", name}, {"enabled", !enabled}}.dump());
        if (!answer) {
            common::DefaultLogger().warn("mcp toggle for '{}' failed: no answer", name);
        } else if (const auto doc = nlohmann::json::parse(*answer, nullptr, false);
                   doc.is_object() && !doc.value("ok", false)) {
            common::DefaultLogger().warn("mcp toggle for '{}' failed: {}", name,
                                         doc.value("error", std::string()));
        }
        FetchList();
    });
}

void McpFeed::FetchList() {
    const auto answer = api_->PluginCall(kServerPlugin, "list", "{}");
    auto servers = answer ? ParseServerList(*answer) : std::nullopt;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (servers) {
            servers_ = std::move(*servers);
            status_.clear();
        } else {
            // No answer: the server-side mcp plugin is disabled or absent (or
            // the call failed). Say so rather than showing an empty, healthy
            // list.
            status_ = "mcp plugin unavailable";
        }
        ++generation_;
    }
    if (ui_ != nullptr) ui_->RequestRedraw();   // outside mu_: host code
}

std::uint64_t McpFeed::generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
}

std::vector<McpServerStatus> McpFeed::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return servers_;
}

std::string McpFeed::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

McpPickerView::McpPickerView(plugins::ClientApi* api, plugins::HostUi* ui)
    : feed_(std::make_shared<McpFeed>(api, ui)), ui_(ui) {}

const plugins::ViewSpec& McpPickerView::spec() const {
    static const plugins::ViewSpec kSpec{"mcp", "mcp servers", plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component McpPickerView::component() {
    return ftxui::Make<McpPickerComponent>(feed_, &theme_, ui_);
}

}  // namespace minimal::mcp_view
