#include "minimal/client/view/status_bar.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {
// {session} duplicates the tab bar's own label; capped independently even
// when the terminal has room to spare, so it never dominates the bar.
constexpr std::size_t kSessionColumnCap = 24;

// Which segment gives way first when the bar overflows: lowest first. The
// live, decision-relevant state (mode, activity, model, context) outranks
// reference detail the user can get elsewhere (version, the tab bar's own
// session name, the cost breakdown).
int SegmentPriority(std::string_view id) {
    if (id == "permission_mode") return 100;
    if (id == "activity") return 90;
    if (id == "model") return 80;
    if (id == "tokens") return 70;
    if (id == "agent") return 60;
    if (id == "cost") return 40;
    // Decision-relevant like the mode: a paused session is not running, and
    // knowing why nothing is happening outranks reference detail.
    if (id == "paused") return 50;
    if (id == "session") return 20;
    if (id == "token_detail") return 15;
    if (id == "version") return 10;
    return 30;   // a plugin segment: below core state, above pure reference
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

std::string CompactCount(std::int64_t n) {
    // One decimal below 10 of a unit ("4.2k"), none above ("62k"); a
    // trailing ".0" is dropped ("2M", not "2.0M").
    const auto scaled = [](std::int64_t value, std::int64_t unit, const char* suffix) {
        if (value < 10 * unit) {
            const std::int64_t tenths = value * 10 / unit;
            std::string out = std::to_string(tenths / 10);
            if (tenths % 10 != 0) out += "." + std::to_string(tenths % 10);
            return out + suffix;
        }
        return std::to_string(value / unit) + suffix;
    };
    if (n < 1000) return std::to_string(n);
    if (n < 1'000'000) return scaled(n, 1000, "k");
    return scaled(n, 1'000'000, "M");
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
    if (max <= 0) return CompactCount(used) + " tok";
    return CompactCount(used) + "/" + CompactCount(max) + " · " +
           std::to_string(used * 100 / max) + "%";
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
                                            int max_width) {
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
            others < max_width_u ? std::min(kSessionColumnCap, max_width_u - others) : 0;
        if (budget == 0) {
            DropSegment(parts, index);
        } else if (own > budget) {
            session->text = TruncateUtf8Columns(session->text, budget);
        } else if (own > kSessionColumnCap) {
            session->text = TruncateUtf8Columns(session->text, kSessionColumnCap);
        }
    }

    while (PartsWidth(parts) > max_width_u) {
        std::optional<std::size_t> victim;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].is_literal || parts[i].is_split) continue;
            if (!victim || SegmentPriority(parts[i].segment_id) <
                               SegmentPriority(parts[*victim].segment_id)) {
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
                               const std::map<std::string, ftxui::Color>& emphasis) {
    using namespace ftxui;
    // One column of padding at each edge, so values never touch the frame.
    const auto parts = ResolveStatusBar(format, values, max_width > 2 ? max_width - 2 : max_width);
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
