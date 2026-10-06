#include "minimal/client/keymap_help_model.hpp"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

#include "minimal/client/utf8.hpp"
#include "minimal/client/policy.hpp"

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
    {"open", {"session.picker", "agent.picker", "model.picker", "help.overlay"}},
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
    {"prompt.newline", "insert a newline"},
    {"prompt.undo", "undo the last prompt edit"},
    {"prompt.redo", "redo the undone prompt edit"},
    {"prompt.clear", "clear the prompt"},
    {"turn.cancel", "interrupt the turn"},
    {"permission.cycle", "cycle the permission mode"},
    {"session.pause", "pause / resume the agent (stops at the next tool boundary)"},
    {"session.picker", "open or resume a session"},
    {"agent.picker", "switch agent"},
    {"model.picker", "switch model"},
    {"help.overlay", "show this list"},
    {"panel.toggle", "show / hide the tools panel"},
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

bool MatchesFilter(const std::string& action, const std::string& key,
                   const std::string& description, const std::string& lowered_filter) {
    if (lowered_filter.empty()) return true;
    return ToLower(action).find(lowered_filter) != std::string::npos ||
           ToLower(key).find(lowered_filter) != std::string::npos ||
           ToLower(description).find(lowered_filter) != std::string::npos;
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

std::vector<KeymapHelpRow> BuildKeymapHelpRows(
    const std::unordered_map<std::string, std::string>& bindings, const std::string& filter) {
    const std::string lowered_filter = ToLower(filter);

    struct Match {
        std::size_t group;
        std::size_t order;
        std::string action;
        std::string key;
        std::string description;
    };
    std::vector<Match> matches;
    std::size_t key_width = 0;
    for (const auto& [action, key] : bindings) {
        std::string description = ActionDescription(action);
        if (!MatchesFilter(action, key, description, lowered_filter)) continue;
        key_width = std::max(key_width, Utf8ColumnWidth(key));
        matches.push_back(
            {GroupOf(action), OrderOf(action), action, key, std::move(description)});
    }
    key_width = std::min(key_width, kKeyColumnCap);
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        return std::tie(a.group, a.order, a.action) < std::tie(b.group, b.order, b.action);
    });

    std::vector<KeymapHelpRow> rows;
    std::size_t current_group = kPluginGroup + 1;   // no header emitted yet
    for (auto& match : matches) {
        if (match.group != current_group) {
            current_group = match.group;
            // One space in from the frame; the menu indents the rows under it.
            rows.push_back({.label = std::string(" ") + (current_group < kGroups.size()
                                                             ? kGroups[current_group].name
                                                             : "plugins"),
                            .header = true});
        }
        std::string key = Utf8ColumnWidth(match.key) > key_width
                              ? TruncateUtf8Columns(match.key, key_width)
                              : match.key;
        key += std::string(key_width - std::min(key_width, Utf8ColumnWidth(key)), ' ');
        rows.push_back({.label = key + "  " + match.description, .action = std::move(match.action)});
    }
    return rows;
}

}  // namespace minimal::client
