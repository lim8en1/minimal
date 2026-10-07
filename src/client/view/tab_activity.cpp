#include "minimal/client/view/tab_activity.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "minimal/client/view/live_tool_row.hpp"

namespace minimal::client {

const char* StateWord(SessionStateKind state) {
    switch (state) {
        case SessionStateKind::Thinking: return "thinking";
        case SessionStateKind::Working: return "working";
        case SessionStateKind::Waiting: return "waiting";
        case SessionStateKind::Error: return "error";
        case SessionStateKind::Done: return "done";
        case SessionStateKind::Idle: return "idle";
    }
    return "idle";
}

namespace {

// Everything a snapshot shows except the tool rows, which ToolsVersion covers.
std::uint64_t KeyOf(const AppState& app, const std::vector<AppState::TabSnapshot>& tabs) {
    std::uint64_t key = app.ToolsVersion();
    const auto mix = [&key](std::uint64_t value) {
        key ^= value + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);  // NOLINT(readability-magic-numbers): boost::hash_combine
    };
    const std::hash<std::string> hash;
    mix(tabs.size());
    for (const auto& tab : tabs) {
        mix(hash(tab.tab_id));
        mix(hash(tab.display_name));
        mix(hash(tab.session_id));
        mix(static_cast<std::uint64_t>(tab.state));
        mix(static_cast<std::uint64_t>(tab.attention_glyph));
        mix((tab.is_focused ? 1U : 0U) | (tab.is_subagent ? 2U : 0U) | (tab.view_only ? 4U : 0U));
    }
    return key;
}


}  // namespace

plugins::ToolActivity ToolActivityOf(const ToolExecutionState& tool) {
    plugins::ToolActivity out;
    out.name = LiveToolName(tool);
    out.summary = LiveToolSummary(tool);
    out.running = tool.started;
    out.failed = !tool.started && tool.is_error;
    out.started_at_unix_ms = tool.started_at_unix_ms.count();
    out.duration_ms = tool.started ? -1 : tool.duration.count();
    out.output = tool.buffered_output;
    out.tool_use_id = tool.tool_use_id;
    out.input = tool.input;
    return out;
}

plugins::TabActivity ActivityOf(const AppState::TabSnapshot& tab) {
    plugins::TabActivity activity;
    activity.tab_id = tab.tab_id;
    activity.session_id = tab.session_id;
    activity.display_name = tab.display_name;
    activity.state = StateWord(tab.state);
    activity.attention = tab.attention_glyph != '\0';
    activity.focused = tab.is_focused;
    activity.subagent = tab.is_subagent;
    activity.view_only = tab.view_only;
    activity.root_ordinal = tab.root_ordinal;
    activity.spawn_seq = tab.spawn_seq;
    for (const auto& tool : tab.tools) activity.tools.push_back(ToolActivityOf(tool));
    return activity;
}

plugins::TabBarBadge ComputeTabBarBadge(const std::vector<AppState::TabSnapshot>& all,
                                        const std::vector<AppState::TabSnapshot>& visible,
                                        TabFilter filter) {
    if (filter == TabFilter::All) return {};
    plugins::TabBarBadge badge;
    badge.active = true;
    badge.label = filter == TabFilter::Group ? "group" : "agents";
    badge.hidden = all.size() >= visible.size() ? all.size() - visible.size() : 0;
    for (const auto& tab : all) {
        if (tab.attention_glyph == '\0') continue;
        const bool shown = std::any_of(visible.begin(), visible.end(),
                                       [&](const auto& v) { return v.tab_id == tab.tab_id; });
        if (!shown) ++badge.attention;
    }
    return badge;
}

std::shared_ptr<const plugins::TabActivitySnapshot> TabActivityFeed::Get(const AppState& app) {
    const std::uint64_t key = KeyOf(app, app.GetAllTabs(/*include_tool_output=*/false));
    if (last_ != nullptr && key == key_) return last_;
    auto snapshot = std::make_shared<plugins::TabActivitySnapshot>();
    snapshot->version = last_ != nullptr ? last_->version + 1 : 1;
    for (const auto& tab : app.GetAllTabs(/*include_tool_output=*/true)) {
        snapshot->tabs.push_back(ActivityOf(tab));
    }
    key_ = key;
    last_ = std::move(snapshot);
    return last_;
}

}  // namespace minimal::client
