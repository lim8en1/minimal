#pragma once

// The /mcp picker, as a Modal plugin view with id "mcp" -- the view_hint the
// server-side mcp plugin's bare `/mcp` answers with. A flat filterable list
// over the mcp plugin's endpoint ("mcp.endpoint": `list`, `set_enabled`);
// Enter on a row toggles that server's enabled flag live, then re-fetches so
// the list shows the server's actual post-toggle status rather than an
// optimistic local flip. Escape is the host's (it closes every plugin modal).
//
// Every PluginCall runs on the view's own worker thread -- the UI thread
// never blocks on the server. OnShow queues a fresh fetch; the component
// rebuilds its menu at render time when a newer snapshot has landed.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>

#include "mcp_picker_model.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"

namespace minimal::mcp_view {

namespace plugins = minimal::client::plugins;

// The data half: the worker, the queue, the latest snapshot. Shared between
// the view and its component (the component outlives nothing; both are
// process-lifetime, this .so is never unloaded).
class McpFeed {
public:
    // `ui` (optional): asked for a repaint whenever a fetch lands, so the
    // picker leaves "loading" as soon as the list arrives.
    McpFeed(plugins::ClientApi* api, plugins::HostUi* ui = nullptr);
    ~McpFeed();

    void Refresh();
    // Flips `name`'s enabled flag (from the latest snapshot), then refreshes.
    void Toggle(const std::string& name);

    // The latest snapshot and its generation (bumped per landed fetch).
    std::uint64_t generation() const;
    std::vector<McpServerStatus> snapshot() const;
    std::string status() const;  // "" | "loading" | an error line

private:
    void Post(std::function<void()> task);
    void Run();
    void FetchList();

    plugins::ClientApi* api_;
    plugins::HostUi* ui_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false;
    std::vector<McpServerStatus> servers_;
    std::uint64_t generation_ = 0;
    std::string status_;
    std::thread worker_;  // last: starts after every member above exists
};

class McpPickerView final : public plugins::View {
public:
    McpPickerView(plugins::ClientApi* api, plugins::HostUi* ui = nullptr);

    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    void OnShow() const override { feed_->Refresh(); }

private:
    std::shared_ptr<McpFeed> feed_;
    client::Theme theme_;
};

}  // namespace minimal::mcp_view
