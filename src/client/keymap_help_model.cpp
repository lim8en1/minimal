#include "minimal/client/keymap_help_model.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {

bool MatchesFilter(const std::string& action, const std::string& key,
                   const std::string& lowered_filter) {
    if (lowered_filter.empty()) return true;
    return ToLower(action).find(lowered_filter) != std::string::npos ||
           ToLower(key).find(lowered_filter) != std::string::npos;
}

}  // namespace

std::vector<KeymapHelpRow> BuildKeymapHelpRows(
    const std::unordered_map<std::string, std::string>& bindings, const std::string& filter) {
    const std::string lowered_filter = ToLower(filter);

    std::vector<KeymapHelpRow> rows;
    rows.reserve(bindings.size());
    for (const auto& [action, key] : bindings) {
        if (!MatchesFilter(action, key, lowered_filter)) continue;
        rows.push_back(KeymapHelpRow{action + "  --  " + key, action});
    }
    std::sort(rows.begin(), rows.end(), [](const KeymapHelpRow& a, const KeymapHelpRow& b) {
        return a.action < b.action;
    });
    return rows;
}

}  // namespace minimal::client
