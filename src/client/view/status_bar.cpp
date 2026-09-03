#include "minimal/client/view/status_bar.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {
// {session} duplicates the tab bar's own label; capped independently even
// when the terminal has room to spare, so it never dominates the bar.
constexpr std::size_t kSessionColumnCap = 24;
}  // namespace

bool IsKnownStatusSegment(std::string_view id) {
    return id == "version" || id == "model" || id == "tokens" || id == "agent" ||
           id == "permission_mode" || id == "session" || id == "activity";
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

std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width) {
    std::vector<StatusBarPart> parts;
    std::string pending_literal;
    int session_index = -1;  // index into `parts` holding {session}'s value, if any
    for (const auto& part : format) {
        if (!part.is_segment) {
            pending_literal += part.text;
            continue;
        }
        // Unknown or unfilled: NOTHING. A missing value silently shrinking the
        // bar beats a placeholder that reads like real state. A literal
        // separator queued around a skipped segment is dropped too (cleared
        // here, not just left unflushed) -- it's only emitted once confirmed
        // to sit between two segments that both actually rendered, so a
        // skipped segment never leaves a doubled " | " behind it.
        const auto it = values.find(part.text);
        if (it == values.end() || it->second.empty()) {
            pending_literal.clear();
            continue;
        }
        if (!parts.empty() && !pending_literal.empty()) {
            parts.push_back({/*is_literal=*/true, pending_literal});
        }
        pending_literal.clear();
        if (part.text == "session") session_index = static_cast<int>(parts.size());
        parts.push_back({/*is_literal=*/false, it->second});
    }

    if (max_width > 0 && session_index >= 0) {
        std::size_t fixed_width = 0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (static_cast<int>(i) != session_index) fixed_width += Utf8ColumnWidth(parts[i].text);
        }
        const std::size_t max_width_u = static_cast<std::size_t>(max_width);
        const std::size_t budget = fixed_width < max_width_u
                                       ? std::min(kSessionColumnCap, max_width_u - fixed_width)
                                       : 0;
        if (budget == 0) {
            // No room at all: drop {session}, and the literal queued right
            // before it -- otherwise that separator dangles next to whatever
            // follows, the same orphan-literal case handled above.
            const bool has_leading_literal =
                session_index > 0 && parts[static_cast<std::size_t>(session_index) - 1].is_literal;
            const auto first = parts.begin() + (has_leading_literal ? session_index - 1 : session_index);
            parts.erase(first, parts.begin() + session_index + 1);
        } else {
            std::string& session_text = parts[static_cast<std::size_t>(session_index)].text;
            const std::size_t cap = std::min(budget, kSessionColumnCap);
            if (Utf8ColumnWidth(session_text) > cap) {
                session_text = TruncateUtf8Columns(session_text, cap);
            }
        }
    }
    return parts;
}

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width) {
    using namespace ftxui;
    const auto parts = ResolveStatusBar(format, values, max_width);
    if (parts.empty()) return emptyElement();
    Elements elements;
    for (const auto& part : parts) {
        elements.push_back(
            text(part.text) |
            color(theme.ColorFor(part.is_literal ? "system_message" : "assistant_message")));
    }
    // filler(), not a second hbox: it takes the row's remaining flex space
    // inside the SAME hbox the bgcolor decorates below, so the background
    // spans the full terminal width instead of just the text's own extent.
    elements.push_back(filler());
    auto bar = hbox(std::move(elements));
    bar |= bgcolor(theme.ColorFor("status_bar_bg"));
    return bar;
}

}  // namespace minimal::client
