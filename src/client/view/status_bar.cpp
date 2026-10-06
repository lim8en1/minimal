#include "minimal/client/view/status_bar.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"
#include "minimal/common/count_format.hpp"
#include "minimal/client/policy.hpp"
#include "minimal/common/units.hpp"

namespace minimal::client {

namespace {
// {session} duplicates the tab bar's own label; capped independently
// (kStatusSessionColumnCap) even when the terminal has room to spare, so it
// never dominates the bar.

// Which segment gives way first when the bar overflows: lowest first
// (kStatusSegmentPriorities). A plugin segment ranks by its own priority.
int SegmentPriority(std::string_view id, const std::map<std::string, int>& priorities) {
    if (const auto it = priorities.find(std::string(id)); it != priorities.end()) return it->second;
    for (const auto& [core_id, priority] : kStatusSegmentPriorities) {
        if (id == core_id) return priority;
    }
    return kPluginStatusSegmentPriority;
}

std::size_t PartsWidth(const std::vector<StatusBarPart>& parts) {
    std::size_t width = 0;
    for (const auto& part : parts) width += Utf8ColumnWidth(part.text);
    return width;
}

// Removes the value at `index` and the ONE literal that joined it to its
// group: the one before it, or -- when it leads its group (the bar's start,
// or right after the split) -- the one after it. Never both, or the two
// neighbors would fuse with no separator between them.
void DropSegment(std::vector<StatusBarPart>& parts, std::size_t index) {
    std::size_t first = index;
    std::size_t last = index;
    if (index > 0 && parts[index - 1].is_literal) {
        first = index - 1;
    } else if (index + 1 < parts.size() && parts[index + 1].is_literal) {
        last = index + 1;
    }
    parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(first),
                parts.begin() + static_cast<std::ptrdiff_t>(last) + 1);
}

}  // namespace

bool IsKnownStatusSegment(std::string_view id) {
    return id == "version" || id == "model" || id == "tokens" || id == "agent" ||
           id == "permission_mode" || id == "session" || id == "activity" ||
           id == "paused" || id == kStatusSplitId;
}

std::vector<std::string> UnknownStatusSegments(const std::string& format) {
    std::vector<std::string> unknown;
    for (const auto& part : ParseStatusFormat(format)) {
        if (part.is_segment && !part.optional && !IsKnownStatusSegment(part.text)) {
            unknown.push_back(part.text);   // registry (E5) or typo -- named either way
        }
    }
    return unknown;
}

std::string FormatTokenUsage(std::int64_t used, std::int64_t max) {
    if (used < 0) used = 0;
    if (max <= 0) return common::CompactCount(used) + " tok";
    return common::CompactCount(used) + "/" + common::CompactCount(max) + " · " +
           std::to_string(common::AsPercent(used, max)) + "%";
}

std::string PermissionModeSegmentValue(const std::string& applied,
                                       const std::string& desired, bool has_session) {
    // The applied value is wire truth and renders alone unless a queued
    // switch is pending. A sessionless tab has nothing to apply into (the
    // create carries no run mode), so there is no pending state to show --
    // same rule the {model}/{agent} markers follow.
    if (desired.empty() || !has_session || desired == applied) return applied;
    return "→ " + desired;
}

std::string WithHiddenRowsSuffix(std::string activity) {
    // The literal is the same " · " spelling the bar renders between
    // segments, so the suffix reads as one composed value.
    constexpr std::string_view kSeparator = " · ";
    constexpr std::string_view kSuffix = "earlier rows hidden";
    if (activity.size() >= kSuffix.size() &&
        activity.compare(activity.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return activity;   // already suffixed -- never double-append
    }
    if (activity.empty()) return std::string(kSuffix);
    activity += kSeparator;
    activity += kSuffix;
    return activity;
}

std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width,
                                            const std::map<std::string, int>& priorities) {
    std::vector<StatusBarPart> parts;
    std::string pending_literal;
    bool group_empty = true;   // no value yet in the current (left or right) group
    for (const auto& part : format) {
        if (!part.is_segment) {
            pending_literal += part.text;
            continue;
        }
        if (part.text == kStatusSplitId) {
            // The split starts a new group: the literal queued before it
            // joined nothing on its right, and the right group's first value
            // takes no leading separator either.
            pending_literal.clear();
            parts.push_back({.is_split = true});
            group_empty = true;
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
        if (!group_empty && !pending_literal.empty()) {
            parts.push_back({.is_literal = true, .text = pending_literal});
        }
        pending_literal.clear();
        parts.push_back({.text = it->second, .segment_id = part.text});
        group_empty = false;
    }

    if (max_width <= 0) return parts;
    const std::size_t max_width_u = static_cast<std::size_t>(max_width);

    const auto session = std::find_if(parts.begin(), parts.end(), [](const StatusBarPart& p) {
        return p.segment_id == "session";
    });
    if (session != parts.end()) {
        const std::size_t index = static_cast<std::size_t>(session - parts.begin());
        const std::size_t own = Utf8ColumnWidth(session->text);
        const std::size_t others = PartsWidth(parts) - own;
        const std::size_t budget =
            others < max_width_u ? std::min(kStatusSessionColumnCap, max_width_u - others) : 0;
        if (budget == 0) {
            DropSegment(parts, index);
        } else if (own > budget) {
            session->text = TruncateUtf8Columns(session->text, budget);
        } else if (own > kStatusSessionColumnCap) {
            session->text = TruncateUtf8Columns(session->text, kStatusSessionColumnCap);
        }
    }

    while (PartsWidth(parts) > max_width_u) {
        std::optional<std::size_t> victim;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].is_literal || parts[i].is_split) continue;
            if (!victim || SegmentPriority(parts[i].segment_id, priorities) <
                               SegmentPriority(parts[*victim].segment_id, priorities)) {
                victim = i;
            }
        }
        if (!victim) break;   // only literals left: nothing more to give
        DropSegment(parts, *victim);
    }
    return parts;
}

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width,
                               const std::map<std::string, ftxui::Color>& emphasis,
                               const std::map<std::string, int>& priorities) {
    using namespace ftxui;
    // One column of padding at each edge, so values never touch the frame.
    const auto parts =
        ResolveStatusBar(format, values, max_width > 2 ? max_width - 2 : max_width, priorities);
    if (parts.empty()) return emptyElement();
    const Color literal_color = MutedColor(theme);
    const Color value_color = theme.ColorFor("assistant_message");
    Elements elements{text(" ")};
    bool split = false;
    for (const auto& part : parts) {
        if (part.is_split) {
            // filler(), not a second hbox: it takes the row's remaining flex
            // space inside the SAME hbox the bgcolor decorates below, so the
            // background spans the full width and the right group sits flush
            // right.
            elements.push_back(filler());
            split = true;
            continue;
        }
        Color c = part.is_literal ? literal_color : value_color;
        if (!part.is_literal) {
            if (const auto it = emphasis.find(part.segment_id); it != emphasis.end()) c = it->second;
        }
        elements.push_back(text(part.text) | color(c));
    }
    if (!split) elements.push_back(filler());
    elements.push_back(text(" "));
    auto bar = hbox(std::move(elements));
    bar |= bgcolor(theme.ColorFor("status_bar_bg"));
    return bar;
}

}  // namespace minimal::client
