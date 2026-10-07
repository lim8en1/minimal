#include "config_view.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>

#include "minimal/client/picker_model.hpp"
#include "minimal/client/view/schema_form_model.hpp"
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
    if (applied == "not_running") {
        return {path + " saved -- " + answer.value("reason", std::string("not running")) +
                    "; it takes effect when it starts",
                false};
    }
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
    // Only what needs attention: how many keys a file sets is the steady
    // state, not news (the form's * marks show them per field).
    std::string label = row.value("name", "");
    if (!row.value("enabled", false)) label += "  (disabled)";
    if (const auto pending = row.value("pending", json::array()); !pending.empty()) {
        label += "  · restart pending";
    }
    if (row.contains("error")) label += "  · " + row.value("error", std::string());
    return label;
}

// The model chooser's "leave it empty" row: not a ref (no '/'), so no model
// can collide with it.
constexpr const char* kUnsetRowId = "<unset>";

// The view's root: the plugin list, one plugin's form, or a chooser over one
// field (an enum's choices, the server's models), whichever is showing.
// Implements EscapeAware: the form's inline editor and an open chooser take
// Escape (back to the form) instead of the host closing the view.
class ConfigComponent final : public ftxui::ComponentBase, public client::EscapeAware {
public:
    ConfigComponent(std::shared_ptr<ConfigFeed> feed, plugins::ClientApi* api,
                    const plugins::WidgetKit* widgets, const client::Theme* theme,
                    plugins::HostUi* ui)
        : feed_(std::move(feed)), api_(api), widgets_(widgets), theme_(theme), ui_(ui) {
        form_ = widgets_->SchemaForm(
            [feed = feed_] { return feed->form(); },
            client::SchemaFormCallbacks{
                .on_set = [feed = feed_](const std::string& path,
                                         const std::string& text) { feed->Set(path, text); },
                .on_unset = [feed = feed_](const std::string& path) { feed->Unset(path); },
                .on_back = [feed = feed_] { feed->Back(); },
                // `this` owns form_, so the form never outlives it.
                .on_pick = [this](const std::string& path,
                                  const nlohmann::json& field) { OpenChooser(path, field); },
            },
            *theme_);
    }

    bool WantsEscape() const override {
        if (feed_->open_plugin().empty()) return false;
        if (choosing_) return true;
        const auto* aware = dynamic_cast<const client::EscapeAware*>(form_.get());
        return aware != nullptr && aware->WantsEscape();
    }

private:
    // Picks the screen (rebuilding the list if a newer one landed) -- at the
    // START of a render or an event, and nowhere else. ftxui asks
    // ActiveChild() while it is drawing, and the feed's worker can land a new
    // list at any moment: a rebuild there freed the very list being drawn,
    // and the frame then wrote its layout into freed memory (a crash on "<-",
    // whose refresh answer tends to land mid-frame). `shown_` holds the
    // component until the next Sync, so whatever a frame or an event started
    // on outlives it.
    void Sync() {
        if (feed_->open_plugin().empty()) choosing_ = false;  // went back meanwhile
        if (!choosing_) chooser_ = nullptr;  // not shown: safe to drop
        if (choosing_) {
            shown_ = chooser_;
            return;
        }
        if (!feed_->open_plugin().empty()) {
            shown_ = form_;
            return;
        }
        RebuildListIfStale();
        shown_ = list_;
    }

    // A chooser over `path`: the server's models for a model ref, the
    // choices for an enum. Built here, on the UI thread, inside the form's
    // event; shown from the next Sync. Choosing sends through the feed like
    // any edit, and returns to the form.
    void OpenChooser(const std::string& path, const nlohmann::json& field) {
        const auto close = [this] { choosing_ = false; };
        if (field.value("format", "") == "model_ref") {
            client::ModelSelectorSpec spec;
            spec.window_title = path;
            spec.select_hint = "set";
            spec.fetch = [api = api_](bool refresh, std::string& error) {
                return api->ListModels(refresh, &error);
            };
            spec.redraw = [ui = ui_] {
                if (ui != nullptr) ui->RequestRedraw();
            };
            const std::string empty = field.value("empty_label", "");
            spec.leading_row_label = empty.empty() ? "(empty)" : "empty -- " + empty;
            spec.leading_row_id = kUnsetRowId;
            spec.allow_custom = true;
            chooser_ = widgets_->ModelSelector(
                std::move(spec),
                {.on_select =
                     [this, path, close](const std::string& ref) {
                         if (ref == kUnsetRowId) {
                             feed_->Unset(path);
                         } else {
                             feed_->Set(path, ref);
                         }
                         close();
                     },
                 .on_close = close},
                *theme_);
        } else {
            chooser_ = EnumChooser(path, field, close);
        }
        choosing_ = true;
    }

    ftxui::Component EnumChooser(const std::string& path, const nlohmann::json& field,
                                 const std::function<void()>& close) {
        // The current value, as the form shows it, starts under the cursor.
        std::string current;
        for (const auto& row : client::BuildFormRows(feed_->form().describe)) {
            if (row.path == path && row.value.is_string()) current = row.value.get<std::string>();
        }
        const std::string fallback = field.value("default", json()).is_string()
                                         ? field.at("default").get<std::string>()
                                         : std::string();
        std::vector<std::string> choices;
        for (const auto& choice : field.value("choices", json::array())) {
            if (choice.is_string()) choices.push_back(choice.get<std::string>());
        }
        auto build_rows = [choices, current, fallback](const std::string& filter) {
            const std::string lowered = client::ToLower(filter);
            std::vector<client::MenuRow> rows;
            for (const auto& choice : choices) {
                if (!client::MatchesFilter(lowered, {choice})) continue;
                std::string label = choice;
                if (choice == current) label += "  (current)";
                if (choice == fallback) label += "  (default)";
                rows.push_back({label, choice, /*header=*/false});
            }
            return rows;
        };
        client::FilterableMenuSpec spec{
            .window_title = path,
            .hint_line = " enter: set  ·  type to filter  ·  esc: back",
            .empty_message = " (no choice matches)",
        };
        spec.status.memory = std::make_shared<client::MenuMemory>(client::MenuMemory{"", current});
        return widgets_->FilterableMenuModal(
            build_rows, spec,
            {.on_select =
                 [this, path, close](const client::MenuRow& row) {
                     feed_->Set(path, row.id);
                     close();
                 },
             .on_close = close},
            *theme_);
    }

    ftxui::Element OnRender() override {
        Sync();
        return shown_->Render();
    }
    bool OnEvent(ftxui::Event event) override {
        Sync();
        const ftxui::Component target = shown_;  // a callback may switch screens
        return target->OnEvent(std::move(event));
    }
    bool Focusable() const override { return true; }
    // No side effects: called mid-frame.
    ftxui::Component ActiveChild() override { return shown_; }

    void RebuildListIfStale() {
        const auto state = feed_->list();
        if (list_ && state.generation == built_generation_) return;
        built_generation_ = state.generation;
        auto rows = state.rows;
        auto build_rows = [rows](const std::string& filter) {
            const std::string lowered = client::ToLower(filter);
            std::vector<client::MenuRow> out;
            for (const auto& row : rows) {
                const std::string name = row.value("name", "");
                if (!client::MatchesFilter(lowered, {name})) continue;
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
        list_ = widgets_->FilterableMenuModal(build_rows, spec, callbacks, *theme_);
        Add(list_);
    }

    std::shared_ptr<ConfigFeed> feed_;
    plugins::ClientApi* api_;
    const plugins::WidgetKit* widgets_;
    const client::Theme* theme_;
    plugins::HostUi* ui_;
    ftxui::Component form_;
    ftxui::Component list_;
    ftxui::Component chooser_;  // while choosing_, and until the next Sync after
    bool choosing_ = false;
    ftxui::Component shown_;  // list_, form_ or chooser_, as of the last Sync
    std::uint64_t built_generation_ = 0;
};

}  // namespace

ConfigFeed::ConfigFeed(plugins::ClientApi* api, plugins::HostUi* ui)
    : api_(api), worker_("config-view", [ui] {
          if (ui != nullptr) ui->RequestRedraw();
      }) {}

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
    worker_.Post([this] { FetchList(); });
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
        // The previous answer is dropped, not kept beside the error: rows
        // from before a server restart showed "restart pending" for changes
        // the new server had already applied.
        list_.rows = json::array();
        list_.status = answer.value("error", std::string("unavailable"));
    }
    ++list_.generation;
}

void ConfigFeed::Open(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Reopening the plugin already open (the view shown again): its last
        // description stands while the fresh one is fetched -- no "loading"
        // flash, and the form keeps the selected field.
        const bool reopen = open_ == name && !form_.describe.is_null();
        open_ = name;
        if (!reopen) {
            // The generation keeps counting across plugins: reset to 1, the
            // next plugin's description could land at the generation the
            // form last built at, and the form kept the previous plugin's
            // rows.
            const std::uint64_t generation = form_.generation + 1;
            form_ = client::SchemaFormState{};
            form_.title = name;
            form_.generation = generation;
        }
    }
    worker_.Post([this, name] { FetchDescribe(name, "", ""); });
}

void ConfigFeed::Back() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        open_.clear();
    }
    RefreshList();
}

void ConfigFeed::FetchDescribe(const std::string& name, std::string status,
                               std::string error_path) {
    const json answer = Call("describe", {{"name", name}});
    std::lock_guard<std::mutex> lock(mu_);
    if (open_ != name) return;  // the user went back meanwhile
    form_.busy = false;
    form_.error_path.clear();
    if (answer.contains("error")) {
        // Dropped for the same reason as a failed list: stale values and
        // pending marks must not outlive the server that reported them.
        form_.describe = nullptr;
        form_.status = answer.value("error", std::string());
        form_.status_is_error = true;
    } else {
        form_.describe = answer;
        form_.title = answer.value("file", name);
        form_.status = std::move(status);
        form_.status_is_error = !error_path.empty();
        form_.error_path = std::move(error_path);
    }
    ++form_.generation;
}

void ConfigFeed::Edit(const std::string& method, json request) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mu_);
        name = open_;
        // Until the answer's describe lands: the form's values are stale.
        if (!name.empty()) form_.busy = true;
    }
    if (name.empty()) return;
    request["name"] = name;
    const std::string path = request.value("key", "");
    worker_.Post([this, method, request, name, path] {
        const auto [status, is_error] = Outcome(path, Call(method, request));
        // The new values, and what happened.
        FetchDescribe(name, status, is_error ? path : std::string());
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

ConfigView::ConfigView(plugins::ClientApi* api, const plugins::WidgetKit* widgets,
                       plugins::HostUi* ui)
    : feed_(std::make_shared<ConfigFeed>(api, ui)), api_(api), widgets_(widgets), ui_(ui) {}

const plugins::ViewSpec& ConfigView::spec() const {
    static const plugins::ViewSpec kSpec{kViewId, "plugin configuration",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component ConfigView::component() {
    return ftxui::Make<ConfigComponent>(feed_, api_, widgets_, &theme_, ui_);
}

void ConfigView::OnShow() const {
    const std::string open = feed_->open_plugin();
    if (open.empty()) {
        feed_->RefreshList();
    } else {
        feed_->Open(open);
    }
}

bool ConfigCommand::Run(const std::vector<std::string>& positional, const std::string&) {
    if (!positional.empty()) {
        view_->feed().Open(positional[0]);
    } else {
        view_->feed().Back();
    }
    if (ui_ != nullptr) ui_->ShowView(kViewId);
    return true;
}

}  // namespace minimal::config_view
