#pragma once

// /config -- plugin configuration in the TUI, as a Modal plugin view with id
// "config". Two screens: the configurable plugins (a filterable list from the
// server's `plugins` endpoint: `list`), and one plugin's form
// (client::MakeSchemaForm over `describe`), whose edits go back as `set` /
// `unset` -- validated by the plugin itself, applied live where every changed
// field is live, otherwise reported as waiting for a restart.
//
// Every PluginCall runs on the feed's worker thread; the UI thread only reads
// snapshots. `/config <plugin>` opens straight onto that plugin's form.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <ftxui/component/component.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/schema_form.hpp"

namespace minimal::config_view {

namespace plugins = minimal::client::plugins;

inline constexpr const char* kViewId = "config";

// The data half: the worker, its queue, and the latest answers.
class ConfigFeed {
public:
    ConfigFeed(plugins::ClientApi* api, plugins::HostUi* ui);
    ~ConfigFeed();

    void RefreshList();
    // Shows `name`'s form (fetching its description).
    void Open(const std::string& name);
    void Back();  // to the list
    void Set(const std::string& path, const std::string& text);
    void Unset(const std::string& path);

    struct ListState {
        nlohmann::json rows;  // the endpoint's `list`, configurable plugins only
        std::string status;   // "" | "loading" | an error
        std::uint64_t generation = 0;
    };
    ListState list() const;
    client::SchemaFormState form() const;
    // The plugin whose form is showing; "" on the list screen.
    std::string open_plugin() const;

private:
    void Post(std::function<void()> task);
    void Run();
    void FetchList();
    // Fetches `name`'s description; `status` is the outcome to show with it,
    // an error for `error_path` when that is not "".
    void FetchDescribe(const std::string& name, std::string status, std::string error_path);
    void Edit(const std::string& method, nlohmann::json request);
    // The endpoint's answer, or {"error": ...} when there was none.
    nlohmann::json Call(const std::string& method, const nlohmann::json& request);

    plugins::ClientApi* api_;
    plugins::HostUi* ui_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false;
    ListState list_;
    std::string open_;
    client::SchemaFormState form_;
    std::thread worker_;  // last: starts after every member above exists
};

class ConfigView final : public plugins::View {
public:
    ConfigView(plugins::ClientApi* api, plugins::HostUi* ui);

    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // Re-fetches what is showing: the list, or the open plugin's form.
    void OnShow() const override;

    ConfigFeed& feed() { return *feed_; }

private:
    std::shared_ptr<ConfigFeed> feed_;
    plugins::HostUi* ui_;
    client::Theme theme_;
};

// `/config [plugin]`: opens the view, on the list or straight onto a plugin.
class ConfigCommand final : public plugins::ClientCommand {
public:
    ConfigCommand(ConfigView* view, plugins::HostUi* ui) : view_(view), ui_(ui) {}
    std::string_view name() const override { return "config"; }
    std::string_view summary() const override {
        return "plugin configuration (/config <plugin> to open one)";
    }
    void Run(const std::vector<std::string>& positional, const std::string& raw) override;

private:
    ConfigView* view_;
    plugins::HostUi* ui_;
};

}  // namespace minimal::config_view
