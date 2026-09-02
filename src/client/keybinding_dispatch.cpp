#include "minimal/client/keybinding_dispatch.hpp"

#include "minimal/client/key_event_parser.hpp"

namespace minimal::client {

std::string NextRootGroup(const std::vector<AppState::TabSnapshot>& tabs,
                          const std::string& focused, bool forward) {
    std::vector<std::string> roots;
    for (const auto& tab : tabs) {
        if (!tab.is_subagent) roots.push_back(tab.session_id);
    }
    if (roots.empty()) return "";

    std::size_t idx = 0;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i] == focused) idx = i;
    }
    return roots[(idx + (forward ? 1 : roots.size() - 1)) % roots.size()];
}

std::string NextAttentionTab(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].session_id == focused) {
            start = i;
            break;
        }
    }
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const std::size_t idx = (start + 1 + i) % tabs.size();   // +1: skip current
        if (tabs[idx].attention_glyph == '!') return tabs[idx].session_id;
    }
    return "";
}

bool AnyWorkInFlight(const std::vector<AppState::TabSnapshot>& tabs,
                    const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        for (const auto& tab : tabs) {
            if (tab.session_id != id) continue;
            switch (tab.state) {
                case SessionStateKind::Thinking:
                case SessionStateKind::Working:
                case SessionStateKind::Waiting:
                    return true;
                default:
                    break;
            }
            break;
        }
    }
    return false;
}

ResolvedKeymap ResolvedKeymap::Build(const Keybindings& keys) {
    ResolvedKeymap out;
    for (const auto& [action, key] : keys.all()) {
        auto events = ParseKeySpec(key);
        if (events.empty()) {
            out.unmapped_.emplace_back(action, key);
            continue;
        }
        out.table_.emplace(action, std::move(events));
    }
    return out;
}

bool ResolvedKeymap::Matches(std::string_view action, const ftxui::Event& event) const {
    const auto it = table_.find(std::string(action));
    if (it == table_.end()) return false;   // never bound, or bound to an unparseable key
    for (const auto& candidate : it->second) {
        if (event == candidate) return true;
    }
    return false;
}

std::vector<std::string> TabsToClose(const std::vector<AppState::TabSnapshot>& tabs,
                                     const std::string& sid) {
    std::optional<int64_t> group;
    bool is_root = false;
    for (const auto& tab : tabs) {
        if (tab.session_id == sid) {
            group = tab.root_ordinal;
            is_root = !tab.is_subagent;
        }
    }
    std::vector<std::string> staged;
    for (const auto& tab : tabs) {
        if (!is_root && tab.session_id != sid) continue;
        if (is_root && group.has_value() && tab.root_ordinal != *group) continue;
        staged.push_back(tab.session_id);
    }
    return staged;
}

}  // namespace minimal::client
