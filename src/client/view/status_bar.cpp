#include "minimal/client/view/status_bar.hpp"

namespace minimal::client {

bool IsKnownStatusSegment(std::string_view id) {
    return id == "version" || id == "model" || id == "tokens" || id == "agent" ||
           id == "permission_mode" || id == "session";
}

std::vector<std::string> UnknownStatusSegments(const std::string& format) {
    std::vector<std::string> unknown;
    for (const auto& part : ParseStatusFormat(format)) {
        if (part.is_segment && !IsKnownStatusSegment(part.text)) {
            unknown.push_back(part.text);   // registry (E5) or typo -- named either way
        }
    }
    return unknown;
}

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme) {
    using namespace ftxui;
    Elements parts;
    for (const auto& part : format) {
        if (!part.is_segment) {
            parts.push_back(text(part.text) | color(theme.ColorFor("system_message")));
            continue;
        }
        // Unknown or unfilled: NOTHING. A missing value silently shrinking the
        // bar beats a placeholder that reads like real state.
        const auto it = values.find(part.text);
        if (it == values.end() || it->second.empty()) continue;
        parts.push_back(text(it->second) | color(theme.ColorFor("assistant_message")));
    }
    if (parts.empty()) return emptyElement();
    auto bar = hbox(std::move(parts));
    bar |= bgcolor(theme.ColorFor("status_bar_bg"));
    return bar;
}

}  // namespace minimal::client
