#include "config_view.hpp"

#include <utility>

#include <ftxui/component/component_base.hpp>

#include "minimal/client/view/filterable_menu.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::config_view {

namespace {

using nlohmann::json;

constexpr const char* kServerEndpoint = "plugins";

// What an edit's answer means, for the form's status line.
std::pair<std::string, bool> Outcome(const std::string& path, const json& answer) {
    if (answer.contains("error")) return {answer.value("error", std::string()), true};
    const std::string applied = answer.value("applied", "");
    if (applied == "live") return {path + " saved -- applied, no restart needed", false};
    if (applied == "restart") {
        std::string text = path + " saved -- restart the server to apply";
        if (answer.contains("reconfigure_error")) {
            text += " (the plugin refused it live: " +
                    answer.value("reconfigure_error", std::string()) + ")";
        }
        return {text, false};
    }
    if (const auto unused = answer.value("unused", json::object()); unused.contains(path)) {
        return {path + " saved -- not used while " + unused.at(path).get<std::string>(), false};
    }
    return {path + ": no change", false};
}

// The list screen's label for one plugin.
std::string ListLabel(const json& row) {
    std::string label = row.value("name", "");
    if (!row.value("enabled", false)) label += "  (disabled)";
    if (const auto set = row.value("set", 0); set > 0) label += "  " + std::to_string(set) + " set";
    if (const auto pending = row.value("pending", json::array()); !pending.empty()) {
        label += "  · restart pending";
    }
    if (row.contains("error")) label += "  · " + row.value("error", std::string());
    return label;
}

// The view's root: the plugin list or one plugin's form, whichever the feed
// says is showing. Implements EscapeAware for the form's inline editor.
class ConfigComponent final : public ftxui::ComponentBase, public client::EscapeAware {
public:
    ConfigComponent(std::shared_ptr<ConfigFeed> feed, const client::Theme* theme,
                    plugins::HostUi* ui)
        : feed_(std::move(feed)), theme_(theme), ui_(ui) {
        form_ = client::MakeSchemaForm(
            [feed = feed_] { return feed->form(); },
            client::SchemaFormCallbacks{
                .on_set = [feed = feed_](const std::string& path,
                                         const std::string& text) { feed->Set(path, text); },
                .on_unset = [feed = feed_](const std::string& path) { feed->Unset(path); },
                .on_back = [feed = feed_] { feed->Back(); },
            },
            *theme_);
    }

    bool WantsEscape() const override {
        const auto* aware = dynamic_cast<const client::EscapeAware*>(form_.get());
        return !feed_->open_plugin().empty() && aware != nullptr && aware->WantsEscape();
    }

private:
    ftxui::Component Current() {
        if (!feed_->open_plugin().empty()) return form_;
        RebuildListIfStale();
        return list_;
    }

    ftxui::Element OnRender() override { return Current()->Render(); }
    bool OnEvent(ftxui::Event event) override { return Current()->OnEvent(std::move(event)); }
    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return Current(); }

    void RebuildListIfStale() {
        const auto state = feed_->list();
        if (list_ && state.generation == built_generation_) return;
        built_generation_ = state.generation;
        auto rows = state.rows;
        auto build_rows = [rows](const std::string& filter) {
            std::vector<client::MenuRow> out;
            for (const auto& row : rows) {
                const std::string name = row.value("name", "");
                if (!filter.empty() && name.find(filter) == std::string::npos) continue;
                out.push_back({ListLabel(row), name, /*header=*/false});
            }
            return out;
        };
        client::FilterableMenuSpec spec{
            .window_title = "plugin configuration",
            .hint_line = " enter: open  ·  type to filter  ·  esc: close",
            .status_suffix = [status = state.status](const std::string&) { return status; },
            .empty_message = state.status == "loading" ? " (loading...)"
                                                       : " (no configurable plugins)",
        };
        client::FilterableMenuCallbacks callbacks{
            .on_select =
                [feed = feed_](const client::MenuRow& row) {
                    if (!row.id.empty()) feed->Open(row.id);
                },
            .on_close =
                [ui = ui_] {
                    if (ui != nullptr) ui->CloseView(kViewId);
                },
        };
        if (list_) DetachAllChildren();
        list_ = client::MakeFilterableMenuModal(build_rows, spec, callbacks, *theme_);
        Add(list_);
    }

    std::shared_ptr<ConfigFeed> feed_;
    const client::Theme* theme_;
    plugins::HostUi* ui_;
    ftxui::Component form_;
    ftxui::Component list_;
    std::uint64_t built_generation_ = 0;
};

}  // namespace

ConfigFeed::ConfigFeed(plugins::ClientApi* api, plugins::HostUi* ui)
    : api_(api), ui_(ui), worker_([this] { Run(); }) {}

ConfigFeed::~ConfigFeed() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ConfigFeed::Post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ConfigFeed::Run() {
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
            common::DefaultLogger().warn("config-view: {}", e.what());
        }
        if (ui_ != nullptr) ui_->RequestRedraw();  // outside mu_: host code
    }
}

json ConfigFeed::Call(const std::string& method, const json& request) {
    const auto answer = api_->PluginCall(kServerEndpoint, method, request.dump());
    if (!answer) return {{"error", "no answer from the server"}};
    json doc = json::parse(*answer, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return {{"error", "the server's answer was not JSON"}};
    return doc;
}

void ConfigFeed::RefreshList() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (list_.rows.empty()) list_.status = "loading";
        ++list_.generation;
    }
    Post([this] { FetchList(); });
}

void ConfigFeed::FetchList() {
    const json answer = Call("list", json::object());
    std::lock_guard<std::mutex> lock(mu_);
    if (answer.is_array()) {
        list_.rows = json::array();
        for (const auto& row : answer) {
            if (row.value("configurable", false)) list_.rows.push_back(row);
        }
        list_.status.clear();
    } else {
        list_.status = answer.value("error", std::string("unavailable"));
    }
    ++list_.generation;
}

void ConfigFeed::Open(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        open_ = name;
        form_ = client::SchemaFormState{};
        form_.title = name;
        form_.generation = 1;
    }
    Post([this, name] { FetchDescribe(name, "", false); });
}

void ConfigFeed::Back() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        open_.clear();
    }
    RefreshList();
}

void ConfigFeed::FetchDescribe(const std::string& name, std::string status, bool status_is_error) {
    const json answer = Call("describe", {{"name", name}});
    std::lock_guard<std::mutex> lock(mu_);
    if (open_ != name) return;  // the user went back meanwhile
    if (answer.contains("error")) {
        form_.status = answer.value("error", std::string());
        form_.status_is_error = true;
    } else {
        form_.describe = answer;
        form_.title = answer.value("file", name);
        form_.status = std::move(status);
        form_.status_is_error = status_is_error;
    }
    ++form_.generation;
}

void ConfigFeed::Edit(const std::string& method, json request) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mu_);
        name = open_;
    }
    if (name.empty()) return;
    request["name"] = name;
    const std::string path = request.value("key", "");
    Post([this, method, request, name, path] {
        const auto [status, is_error] = Outcome(path, Call(method, request));
        FetchDescribe(name, status, is_error);  // the new values, and what happened
    });
}

void ConfigFeed::Set(const std::string& path, const std::string& text) {
    Edit("set", {{"key", path}, {"text", text}});
}

void ConfigFeed::Unset(const std::string& path) { Edit("unset", {{"key", path}}); }

ConfigFeed::ListState ConfigFeed::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    return list_;
}

client::SchemaFormState ConfigFeed::form() const {
    std::lock_guard<std::mutex> lock(mu_);
    return form_;
}

std::string ConfigFeed::open_plugin() const {
    std::lock_guard<std::mutex> lock(mu_);
    return open_;
}

ConfigView::ConfigView(plugins::ClientApi* api, plugins::HostUi* ui)
    : feed_(std::make_shared<ConfigFeed>(api, ui)), ui_(ui) {}

const plugins::ViewSpec& ConfigView::spec() const {
    static const plugins::ViewSpec kSpec{kViewId, "plugin configuration",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component ConfigView::component() {
    return ftxui::Make<ConfigComponent>(feed_, &theme_, ui_);
}

void ConfigView::OnShow() const {
    const std::string open = feed_->open_plugin();
    if (open.empty()) {
        feed_->RefreshList();
    } else {
        feed_->Open(open);
    }
}

void ConfigCommand::Run(const std::vector<std::string>& positional, const std::string&) {
    if (!positional.empty()) {
        view_->feed().Open(positional[0]);
    } else {
        view_->feed().Back();
    }
    if (ui_ != nullptr) ui_->ShowView(kViewId);
}

}  // namespace minimal::config_view
