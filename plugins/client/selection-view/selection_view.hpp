#pragma once

// selection-view -- the choosers for what a session runs with, as the views
// core opens by role (and the server's view_hints name): "agents" (the
// project's agent presets), "models" (the server's models, or back to the
// default) and "subagent-models" (the focused session's subagent model, or
// the parent's). The commands and keys that open them are core's (/agent,
// /model, /subagent-model, alt+g, alt+m), and so is what a choice does:
// each pick is a HostActions verb.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/plugins/task_worker.hpp"
#include "minimal/client/render/theme.hpp"

namespace minimal::selection_view {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

inline constexpr const char* kAgentsViewId = "agents";
inline constexpr const char* kModelsViewId = "models";
inline constexpr const char* kSubagentModelsViewId = "subagent-models";

// The agent picker's rows: "name  ·  description", in the server's order,
// those whose name or description contains `filter` (case-insensitively);
// each in the preset's own color.
std::vector<client::MenuRow> BuildAgentRows(const std::vector<v1::AgentPresetSummary>& presets,
                                            const std::string& filter);

// The host seams the views use (non-null; host-owned).
struct Seams {
    plugins::HostUi* ui;
    plugins::HostActions* actions;
    const plugins::WidgetKit* widgets;
    plugins::ClientApi* api;
};

// What the agents view's fetch publishes (worker) and its menu reads (UI).
struct AgentsState;

class AgentsView final : public plugins::View {
public:
    explicit AgentsView(Seams seams);
    ~AgentsView() override;
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // A fresh fetch, and a fresh filter.
    void OnShow() const override;

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<AgentsState> state_;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<plugins::TaskWorker> worker_;  // last: joined first
};

// "models" and "subagent-models": the kit's model selector, rebuilt (and
// so re-fetched) on every open.
class ModelsView final : public plugins::View {
public:
    // `for_subagents`: the subagent-model form -- a "same as parent" row,
    // and a pick sets the session's subagent model.
    ModelsView(Seams seams, bool for_subagents) : seams_(seams), for_subagents_(for_subagents) {}
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    void OnShow() const override { stale_->store(true); }

private:
    Seams seams_;
    bool for_subagents_;
    client::Theme theme_;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace minimal::selection_view
