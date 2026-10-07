#include "minimal/client/keymap_help_model.hpp"

#include <algorithm>
#include <array>


namespace minimal::client {

namespace {

// The overlay's groups, in display order; an action belongs to the first
// whose prefix it starts with (exact ids listed for the "open" group, whose
// actions share no prefix). Anything unmatched is a plugin's.
struct Group {
    const char* name;
    std::array<const char*, 4> prefixes;
};
constexpr std::array<Group, 7> kGroups = {{
    {"tabs", {"tab.", "group.", "jump.", nullptr}},
    {"chat", {"chat.", nullptr, nullptr, nullptr}},
    {"prompt", {"prompt.", nullptr, nullptr, nullptr}},
    {"turn", {"turn.", "permission.", "session.pause", nullptr}},
    {"open", {"session.picker", "agent.picker", "model.picker", nullptr}},
    {"panels", {"panel.", nullptr, nullptr, nullptr}},
    {"app", {"app.", nullptr, nullptr, nullptr}},
}};
constexpr std::size_t kPluginGroup = kGroups.size();

// Every core action, in the order it reads best within its group (next
// before previous, the everyday keys before the rare ones) -- alphabetical
// order put "next tab" below "go to tab 9". Also the description table.
struct KnownAction {
    std::string_view action;
    std::string_view description;
};
constexpr KnownAction kKnown[] = {
    {"tab.next", "next tab"},
    {"tab.previous", "previous tab"},
    {"tab.goto.1", "go to tab 1"},
    {"tab.goto.2", "go to tab 2"},
    {"tab.goto.3", "go to tab 3"},
    {"tab.goto.4", "go to tab 4"},
    {"tab.goto.5", "go to tab 5"},
    {"tab.goto.6", "go to tab 6"},
    {"tab.goto.7", "go to tab 7"},
    {"tab.goto.8", "go to tab 8"},
    {"tab.goto.9", "go to tab 9"},
    {"tab.close", "close the tab"},
    {"group.next", "next tab group"},
    {"group.previous", "previous tab group"},
    {"tab.filter_group", "show only the focused tab's group"},
    {"tab.filter_agents", "show only agent tabs (hide subagents)"},
    {"jump.attention", "jump to the tab that needs you"},
    {"chat.scroll_up", "scroll up"},
    {"chat.scroll_down", "scroll down"},
    {"chat.scroll_bottom", "jump to the newest message"},
    {"chat.load_older", "load earlier messages"},
    {"chat.row_up", "row cursor up"},
    {"chat.row_down", "row cursor down"},
    {"chat.toggle_collapse", "expand / collapse the row"},
    {"chat.toggle_collapse_all", "expand / collapse every row"},
    {"chat.toggle_thinking_fold", "auto-fold / unfold thinking rows"},
    {"chat.toggle_tool_fold", "auto-fold / unfold tool rows"},
    {"chat.toggle_timestamps", "show / hide timestamps"},
    {"turn.cancel", "interrupt the turn"},
    {"permission.cycle", "cycle the permission mode"},
    {"session.pause", "pause / resume the agent (stops at the next tool boundary)"},
    {"session.picker", "open or resume a session"},
    {"agent.picker", "switch agent"},
    {"model.picker", "switch model"},
    {"panel.focus.next", "focus the next panel"},
    {"panel.focus.prev", "focus the previous panel"},
    {"app.quit", "quit"},
};
constexpr std::size_t kUnknownOrder = std::size(kKnown);

std::size_t OrderOf(std::string_view action) {
    for (std::size_t i = 0; i < std::size(kKnown); ++i) {
        if (kKnown[i].action == action) return i;
    }
    return kUnknownOrder;   // plugin actions: after the known ones, by id
}

std::size_t GroupOf(const std::string& action) {
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        for (const char* prefix : kGroups[g].prefixes) {
            if (prefix != nullptr && action.rfind(prefix, 0) == 0) return g;
        }
    }
    return kPluginGroup;
}

}  // namespace

std::string ActionDescription(std::string_view action) {
    for (const auto& known : kKnown) {
        if (known.action == action) return std::string(known.description);
    }
    constexpr std::string_view kGoto = "tab.goto.";   // beyond 9, should one be bound
    if (action.substr(0, kGoto.size()) == kGoto) {
        return "go to tab " + std::string(action.substr(kGoto.size()));
    }
    return std::string(action);
}

KeyActionInfo DescribeKeyAction(std::string_view action, std::optional<std::size_t> rank) {
    const std::string id(action);
    const std::size_t group = GroupOf(id);
    const std::size_t within = rank.has_value() ? std::min(*rank, kUnknownOrder) : OrderOf(action);
    return {ActionDescription(action),
            group < kGroups.size() ? kGroups[group].name : "views",
            static_cast<int>(group * (kUnknownOrder + 1) + within)};
}

}  // namespace minimal::client
