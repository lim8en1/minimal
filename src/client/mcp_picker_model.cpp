#include "minimal/client/mcp_picker_model.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

std::vector<McpPickerRow> BuildMcpPickerRows(const std::vector<v1::McpServerSummary>& servers,
                                             const std::string& filter) {
    const std::string lowered_filter = ToLower(filter);

    std::vector<const v1::McpServerSummary*> ordered;
    ordered.reserve(servers.size());
    for (const auto& server : servers) ordered.push_back(&server);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const v1::McpServerSummary* a, const v1::McpServerSummary* b) {
                         return a->name() < b->name();
                     });

    std::vector<McpPickerRow> rows;
    for (const auto* summary : ordered) {
        if (!lowered_filter.empty() &&
            ToLower(summary->name()).find(lowered_filter) == std::string::npos) {
            continue;
        }
        std::string label = summary->name() + "  [" + summary->transport() + ", ";
        if (!summary->enabled()) {
            label += "disabled]";
        } else if (summary->alive()) {
            label += "enabled, connected]";
        } else if (!summary->error().empty()) {
            label += "enabled, error: " + summary->error() + "]";
        } else {
            label += "enabled, not yet connected]";
        }
        rows.push_back(McpPickerRow{std::move(label), summary->name()});
    }
    return rows;
}

}  // namespace minimal::client
