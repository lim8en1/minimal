#include "config_view.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>

#include "minimal/client/sdk/picker_model.hpp"
#include "minimal/client/sdk/schema_form_model.hpp"
#include "minimal/common/logger.hpp"
#include "policy.hpp"

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

// "on", "off", or what a toggle waiting for its restart will make it.
std::string OnOff(bool running, bool in_file, const char* restart) {
    if (running == in_file) return running ? "on " : "off";
    return std::string(in_file ? "on " : "off") + " (" + restart + ")";
}

// What a plugin registered, in a few words: a kind's names when there are
// one or two ("provider: openai"), its count past that ("12 commands").
std::string ProvidesSummary(const std::map<std::string, std::vector<std::string>>& provides) {
    std::string out;
    for (const auto& [raw_label, names] : provides) {
        if (names.empty()) continue;
        // Registry labels read as words: "client_commands" -> "client commands".
        std::string label = raw_label;
        std::replace(label.begin(), label.end(), '_', ' ');
        std::string part;
        if (names.size() == 1 && names[0] == raw_label) {
            part = label;   // an exclusive slot names itself: "chat surface"
        } else if (names.size() <= 2) {
            part = label + ": " + names[0] + (names.size() == 2 ? ", " + names[1] : "");
        } else {
            const bool plural = !label.empty() && label.back() == 's';
            part = std::to_string(names.size()) + " " + label + (plural ? "" : "s");
        }
        out += (out.empty() ? "" : " · ") + part;
    }
    return out;
}

std::map<std::string, std::vector<std::string>> ProvidesFrom(const json& provides) {
    std::map<std::string, std::vector<std::string>> out;
    if (!provides.is_object()) return out;
    for (const auto& [label, names] : provides.items()) {
        for (const auto& name : names) {
            if (name.is_string()) out[label].push_back(name.get<std::string>());
        }
    }
    return out;
}

// Names padded to one column, so states and summaries line up.
constexpr std::size_t kNameColumn = 24;  // NOLINT(readability-magic-numbers): the longest bundled name, "compaction-structured", and a gap
std::string Padded(const std::string& name) {
    return name.size() >= kNameColumn ? name + " " : name + std::string(kNameColumn - name.size(), ' ');
}

// The list's row ids: which side a plugin is on, or a core file.
constexpr const char* kServerPrefix = "s:";
constexpr const char* kClientPrefix = "c:";
constexpr const char* kCorePrefix = "k:";

bool IsCore(const json& row) { return row.value("kind", "") == "core"; }

// A core file's row: no on/off, only what needs attention.
std::string CoreLabel(const json& row) {
    std::string label = Padded(row.value("name", ""));
    if (const auto pending = row.value("pending", json::array()); !pending.empty()) {
        label += "  · restart pending";
    }
    if (row.contains("error")) label += "  · " + row.value("error", std::string());
    return label;
}

// The list screen's label for one server plugin. Only what needs attention:
// how many keys a file sets is the steady state, not news (the form's *
// marks show them per field).
std::string ServerLabel(const json& row) {
    const bool enabled = row.value("enabled", false);
    std::string label = Padded(row.value("name", "")) +
                        OnOff(enabled, row.value("enabled_in_file", enabled),
                              "at server restart");
    if (enabled && !row.value("loaded", false)) label += "  · failed to load";
    if (const auto pending = row.value("pending", json::array()); !pending.empty()) {
        label += "  · restart pending";
    }
    // Running, but a core field that would put it to use names another
    // (permissions.json's advisor): enabling it alone does nothing.
    for (const auto& unselected : row.value("unselected", json::array())) {
        label += "  · not the " + unselected.value("field", std::string()) + " in " +
                 unselected.value("file", std::string());
    }
    if (row.contains("error")) label += "  · " + row.value("error", std::string());
    if (const auto checks = row.value("checks", json()); checks.is_object()) {
        if (checks.value("state", "") == "running") {
            label += "  · checking...";
        } else if (const int errors = checks.value("errors", 0); errors > 0) {
            label += "  · ✗ " + std::to_string(errors) + (errors == 1 ? " check failed" : " checks failed");
        } else if (const int warnings = checks.value("warnings", 0); warnings > 0) {
            label += "  · ⚠ " + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
        }
    }
    if (const std::string provides = ProvidesSummary(ProvidesFrom(row.value("provides", json()))); !provides.empty()) {
        label += "  · " + provides;
    }
    return label;
}

std::string ClientLabel(const plugins::ClientPluginInfo& plugin, bool in_file) {
    std::string label = Padded(plugin.name) + OnOff(plugin.enabled, in_file, "at TUI restart");
    if (!plugin.error.empty()) label += "  · failed: " + plugin.error;
    if (const std::string provides = ProvidesSummary(plugin.provides); !provides.empty()) {
        label += "  · " + provides;
    }
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
                    plugins::HostUi* ui, plugins::HostActions* actions)
        : feed_(std::move(feed)),
          api_(api),
          widgets_(widgets),
          theme_(theme),
          ui_(ui),
          actions_(actions) {
        form_ = widgets_->SchemaForm(
            [feed = feed_] { return feed->form(); },
            client::SchemaFormCallbacks{
                .on_set = [feed = feed_](const std::string& path,
                                         const std::string& text) { feed->Set(path, text); },
                .on_unset = [feed = feed_](const std::string& path) { feed->Unset(path); },
                .on_back = [feed = feed_] { feed->Back(); },
                // `this` owns form_, so the form never outlives it.
                .on_pick = [this](const client::PickRequest& request) { OpenChooser(request); },
                .on_check = [feed = feed_] { feed->Check(); },
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
            PollRunningCheck();
            shown_ = form_;
            return;
        }
        RebuildListIfStale();
        shown_ = list_;
    }

    // A chooser for a field: the server's models for a model ref or a
    // model's own name, the choices for an enum. Built here, on the UI
    // thread, inside the form's event; shown from the next Sync. The choice
    // goes back through the request (saved, or into an item's draft), and
    // the form shows again.
    void OpenChooser(const client::PickRequest& request) {
        const auto close = [this] { choosing_ = false; };
        const nlohmann::json& field = request.field;
        // What the user calls it: a map entry's "name", else the field's path.
        const std::string path = field.value("label", request.path);
        const std::string format = field.value("format", "");
        if (format == "model_ref" || format == "model_id") {
            client::ModelSelectorSpec spec;
            spec.window_title = path;
            spec.select_hint = "set";
            spec.fetch = [api = api_](bool refresh, std::string& error) {
                return api->ListModels(refresh, &error);
            };
            spec.post = [feed = feed_](std::function<void()> task) {
                feed->Post(std::move(task));
            };
            spec.redraw = [ui = ui_] {
                if (ui != nullptr) ui->RequestRedraw();
            };
            // A required value has no "empty" to choose.
            if (!field.value("required", false)) {
                const std::string empty = field.value("empty_label", "");
                spec.leading_row_label = empty.empty() ? "(empty)" : "empty -- " + empty;
                spec.leading_row_id = kUnsetRowId;
            }
            spec.allow_custom = true;
            spec.choose_model_name = format == "model_id";
            chooser_ = widgets_->ModelSelector(
                std::move(spec),
                {.on_select =
                     [deliver = request.deliver, close](const std::string& ref) {
                         deliver(ref == kUnsetRowId ? std::nullopt : std::optional<std::string>(ref));
                         close();
                     },
                 .on_close = close},
                *theme_);
        } else {
            chooser_ = EnumChooser(request, close);
        }
        choosing_ = true;
    }

    ftxui::Component EnumChooser(const client::PickRequest& request,
                                 const std::function<void()>& close) {
        const std::string path = request.field.value("label", request.path);
        const nlohmann::json& field = request.field;
        // The current value, as the form shows it, starts under the cursor.
        const std::string current =
            request.current.is_string() ? request.current.get<std::string>() : std::string();
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
                 [deliver = request.deliver, close](const client::MenuRow& row) {
                     deliver(row.id);
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

    // A check in flight on the open plugin: its findings land in a later
    // describe, so ask again each second (and for the frame to show them).
    void PollRunningCheck() {
        const auto form = feed_->form();
        const bool running = form.describe.is_object() && form.describe.contains("checks") &&
                             form.describe.at("checks").is_object() &&
                             form.describe.at("checks").value("state", "") == "running";
        if (!running || form.busy) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check_poll_ >= kCheckPoll) {
            last_check_poll_ = now;
            feed_->Refresh();
        }
        if (ui_ != nullptr) ui_->RequestRedrawAfter(kCheckPoll);
    }

    void RebuildListIfStale() {
        const auto state = feed_->list();
        if (list_ && state.generation == built_generation_ &&
            client_outcome_generation_ == built_client_outcome_) {
            return;
        }
        built_generation_ = state.generation;
        built_client_outcome_ = client_outcome_generation_;
        auto rows = state.rows;
        // Server plugins from the feed; TUI plugins read live from the host
        // (a toggle lands before the next frame), with the toggles made here
        // until the host's list shows them.
        auto build_rows = [rows, ui = ui_, toggled = client_toggled_](const std::string& filter) {
            const std::string lowered = client::ToLower(filter);
            std::vector<client::MenuRow> out;
            // A section's header only over rows the filter kept.
            bool header = false;
            for (const auto& row : rows) {
                const std::string name = row.value("name", "");
                if (!IsCore(row) || !client::MatchesFilter(lowered, {name})) continue;
                if (!std::exchange(header, true)) out.push_back({"core", "", /*header=*/true});
                out.push_back({CoreLabel(row), kCorePrefix + name, /*header=*/false});
            }
            header = false;
            for (const auto& row : rows) {
                const std::string name = row.value("name", "");
                if (IsCore(row) || !client::MatchesFilter(lowered, {name})) continue;
                if (!std::exchange(header, true)) out.push_back({"server plugins", "", /*header=*/true});
                out.push_back({ServerLabel(row), kServerPrefix + name, /*header=*/false});
            }
            const auto client = ui != nullptr ? ui->ClientPlugins()
                                              : std::vector<plugins::ClientPluginInfo>{};
            header = false;
            for (const auto& plugin : client) {
                if (!client::MatchesFilter(lowered, {plugin.name})) continue;
                if (!std::exchange(header, true)) out.push_back({"TUI plugins", "", /*header=*/true});
                const auto it = toggled->find(plugin.id);
                const bool in_file = it != toggled->end() ? it->second : plugin.enabled_in_file;
                out.push_back({ClientLabel(plugin, in_file), kClientPrefix + plugin.id,
                               /*header=*/false});
            }
            return out;
        };
        std::string outcome = !client_outcome_.empty() && client_outcome_generation_ > 0 &&
                                      client_outcome_newer_
                                  ? client_outcome_
                                  : state.outcome;
        client::FilterableMenuSpec spec{
            .window_title = "plugins",
            // The last toggle's outcome leads the footer (a paragraph: it wraps).
            .hint_line = (outcome.empty() ? std::string() : " " + outcome + "  ·  ") +
                         " enter: configure  ·  space: on/off  ·  type to filter  ·  esc: close",
            .status_suffix = [status = state.status](const std::string&) { return status; },
            .empty_message = state.status == "loading" ? " (loading...)" : " (no plugins)",
        };
        spec.status.memory = list_memory_;
        client::FilterableMenuCallbacks callbacks{
            .on_select =
                [feed = feed_, rows](const client::MenuRow& row) {
                    // TUI plugins: no config here.
                    if (row.id.rfind(kServerPrefix, 0) != 0 && row.id.rfind(kCorePrefix, 0) != 0) {
                        return;
                    }
                    const std::string name = row.id.substr(2);
                    for (const auto& server : rows) {
                        if (server.value("name", "") == name && server.value("configurable", false)) {
                            feed->Open(name);
                        }
                    }
                },
            .on_close =
                [ui = ui_] {
                    if (ui != nullptr) ui->CloseView(kViewId);
                },
            .on_extra_key =
                [this, rows](const ftxui::Event& event, const client::MenuContext& ctx,
                             const std::function<void()>&) {
                    if (event != ftxui::Event::Character(' ')) return false;
                    if (ctx.selected < 0 || ctx.selected >= static_cast<int>(ctx.rows.size())) {
                        return true;
                    }
                    Toggle(ctx.rows[ctx.selected].id, rows);
                    return true;
                },
        };
        if (list_) DetachAllChildren();
        list_ = widgets_->FilterableMenuModal(build_rows, spec, callbacks, *theme_);
        Add(list_);
    }

    // Space on a row: flips the plugin's entry for its next start.
    void Toggle(const std::string& id, const nlohmann::json& rows) {
        if (id.rfind(kServerPrefix, 0) == 0) {
            const std::string name = id.substr(2);
            for (const auto& row : rows) {
                if (row.value("name", "") != name) continue;
                const bool enabled = row.value("enabled_in_file", row.value("enabled", false));
                client_outcome_newer_ = false;
                feed_->Toggle(name, !enabled);
            }
            return;
        }
        if (id.rfind(kCorePrefix, 0) == 0) {
            client_outcome_ = id.substr(2) + " is core configuration: it has no on/off";
            client_outcome_newer_ = true;
            ++client_outcome_generation_;
            return;
        }
        if (id.rfind(kClientPrefix, 0) != 0 || ui_ == nullptr) return;
        const std::string plugin_id = id.substr(2);
        for (const auto& plugin : ui_->ClientPlugins()) {
            if (plugin.id != plugin_id) continue;
            const auto it = client_toggled_->find(plugin.id);
            const bool in_file = it != client_toggled_->end() ? it->second : plugin.enabled_in_file;
            if (in_file && !plugin.required.empty()) {
                client_outcome_ = "cannot turn " + plugin.name + " off: " + plugin.required +
                                  " -- the TUI would not start without it";
            } else if (actions_ == nullptr) {
                client_outcome_ = "this TUI does not offer plugin toggles";
            } else {
                actions_->SetClientPluginEnabled(plugin.id, !in_file);
                (*client_toggled_)[plugin.id] = !in_file;
                client_outcome_ = plugin.name + (in_file ? " off" : " on") +
                                  (plugin.enabled == !in_file ? "" : " at the next TUI start");
            }
            client_outcome_newer_ = true;
            ++client_outcome_generation_;
        }
    }

    std::shared_ptr<ConfigFeed> feed_;
    plugins::ClientApi* api_;
    const plugins::WidgetKit* widgets_;
    const client::Theme* theme_;
    plugins::HostUi* ui_;
    plugins::HostActions* actions_;
    // The list's filter and cursor, kept across the rebuild a toggle causes.
    std::shared_ptr<client::MenuMemory> list_memory_ = std::make_shared<client::MenuMemory>();
    std::chrono::steady_clock::time_point last_check_poll_{};
    // TUI plugin toggles made here (id -> on in the file), until the host's
    // own list shows them; and what the last one did.
    std::shared_ptr<std::map<std::string, bool>> client_toggled_ =
        std::make_shared<std::map<std::string, bool>>();
    std::string client_outcome_;
    bool client_outcome_newer_ = false;   // newer than the feed's (server) outcome
    std::uint64_t client_outcome_generation_ = 0;
    std::uint64_t built_client_outcome_ = 0;
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
        list_.rows = answer;   // every plugin: each can be toggled
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

void ConfigFeed::Toggle(const std::string& name, bool enabled) {
    worker_.Post([this, name, enabled] {
        const json answer = Call("enable", {{"name", name}, {"enabled", enabled}});
        std::string outcome;
        if (answer.contains("error")) {
            outcome = answer.value("error", std::string());
        } else {
            outcome = name + (enabled ? " on" : " off") +
                      (answer.value("applied", "") == "restart" ? " at the next server start" : "");
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            list_.outcome = std::move(outcome);
        }
        FetchList();
    });
}

void ConfigFeed::Check() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mu_);
        name = open_;
    }
    if (name.empty()) return;
    worker_.Post([this, name] {
        const json answer = Call("check", {{"name", name}});
        std::string status;
        std::string error_path;
        if (answer.contains("error")) {
            status = answer.value("error", std::string());
            error_path = " ";   // an error, about no field
        }
        FetchDescribe(name, status, error_path);
    });
}

void ConfigFeed::Refresh() {
    std::string name;
    std::string status;
    std::string error_path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        name = open_;
        status = form_.status;
        error_path = form_.status_is_error ? (form_.error_path.empty() ? " " : form_.error_path) : "";
    }
    if (name.empty()) return;
    worker_.Post([this, name, status, error_path] { FetchDescribe(name, status, error_path); });
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
                       plugins::HostUi* ui, plugins::HostActions* actions)
    : feed_(std::make_shared<ConfigFeed>(api, ui)),
      api_(api),
      widgets_(widgets),
      ui_(ui),
      actions_(actions) {}

const plugins::ViewSpec& ConfigView::spec() const {
    static const plugins::ViewSpec kSpec{kViewId, "plugin configuration",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component ConfigView::component() {
    return ftxui::Make<ConfigComponent>(feed_, api_, widgets_, &theme_, ui_, actions_);
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
