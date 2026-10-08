#include "session_status.hpp"
#include "policy.hpp"

#include <functional>
#include <string_view>
#include <utility>

#include "minimal/client/sdk/policy.hpp"
#include "minimal/client/sdk/tool_status.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/count_format.hpp"
#include "minimal/common/units.hpp"

namespace minimal::session_status {

namespace {

// One segment: an id, where it sits, its priority and column cap, and how
// it reads the tab.
class Segment final : public plugins::StatusSegment {
public:
    using Fn = std::function<plugins::StatusValue(const plugins::TabStatus&)>;
    Segment(std::string_view id, std::optional<plugins::StatusPlacement> placement, int priority,
            std::size_t max_columns, Fn render)
        : id_(id),
          placement_(placement),
          priority_(priority),
          max_columns_(max_columns),
          render_(std::move(render)) {}

    std::string_view id() const override { return id_; }
    plugins::StatusValue Render(const plugins::TabStatus& focused) const override {
        return render_(focused);
    }
    std::optional<plugins::StatusPlacement> placement() const override { return placement_; }
    int priority() const override { return priority_; }
    std::size_t max_columns() const override { return max_columns_; }

private:
    std::string_view id_;
    std::optional<plugins::StatusPlacement> placement_;
    int priority_;
    std::size_t max_columns_;
    Fn render_;
};

constexpr plugins::StatusPlacement Left(int order) {
    return {plugins::StatusPlacement::Side::kLeft, order};
}
constexpr plugins::StatusPlacement Right(int order) {
    return {plugins::StatusPlacement::Side::kRight, order};
}

// The desired-state rule for {model} / {agent}: the user's last choice
// shows, with the "→ " pending marker until the session confirms it -- on a
// sessionless tab there is nothing to converge into, so no marker (the
// create simply carries the choice).
std::string Staged(const plugins::TabStatus& tab, const std::string& effective,
                   const std::string& desired) {
    const std::string& value = desired.empty() ? effective : desired;
    if (value.empty()) return {};
    const bool pending = !tab.session_id.empty() && !desired.empty() && desired != effective;
    return (pending ? "→ " : "") + value;
}

}  // namespace

std::string FormatTokenUsage(std::int64_t used, std::int64_t max, bool estimated) {
    if (used < 0) used = 0;
    // An estimate says so: it is bytes/4, which dense output undercounts.
    const std::string count = (estimated ? "~" : "") + common::CompactCount(used);
    if (max <= 0) return count + " tok";
    return count + "/" + common::CompactCount(max) + " · " +
           std::to_string(common::AsPercent(used, max)) + "%";
}

std::string PermissionModeSegmentValue(const std::string& applied, const std::string& desired,
                                       bool has_session) {
    if (desired.empty() || !has_session || desired == applied) return applied;
    return "→ " + desired;
}

std::string WithHiddenRowsSuffix(std::string activity) {
    constexpr std::string_view kSeparator = " · ";
    constexpr std::string_view kSuffix = "earlier rows hidden";
    if (activity.size() >= kSuffix.size() &&
        activity.compare(activity.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return activity;  // already suffixed -- never double-append
    }
    if (activity.empty()) return std::string(kSuffix);
    activity += kSeparator;
    activity += kSuffix;
    return activity;
}

std::vector<std::unique_ptr<plugins::StatusSegment>> MakeSegments() {
    using plugins::StatusValue;
    using plugins::TabStatus;
    std::vector<std::unique_ptr<plugins::StatusSegment>> out;
    const auto add = [&out](std::string_view id, std::optional<plugins::StatusPlacement> placement,
                            int priority, std::size_t cap, Segment::Fn fn) {
        out.push_back(std::make_unique<Segment>(id, placement, priority, cap, std::move(fn)));
    };

    add("version", Left(kVersionOrder), kVersionPriority, 0,
        [](const TabStatus&) { return StatusValue{std::string(common::Version()), ""}; });

    // A name, not a figure: the one segment the bar may cut short.
    add("session", Left(kSessionOrder), kSessionPriority, client::kStatusSessionColumnCap, [](const TabStatus& tab) {
        if (!tab.focused) return StatusValue{};
        std::string name = tab.display_name.empty() ? "new session" : tab.display_name;
        // A read-only transcript says so where the user looks before typing.
        if (tab.read_only) name += "  (read-only)";
        return StatusValue{std::move(name), ""};
    });

    // Tinted as the window fills.
    add("tokens", Left(kTokensOrder), kTokensPriority, 0, [](const TabStatus& tab) {
        if (!tab.focused || tab.tokens_used <= 0) return StatusValue{};
        std::string role;
        if (tab.context_window > 0) {
            const std::int64_t percent = common::AsPercent(tab.tokens_used, tab.context_window);
            if (percent >= kContextErrorPercent) {
                role = "error";
            } else if (percent >= kContextAttentionPercent) {
                role = "attention";
            }
        }
        return StatusValue{
            FormatTokenUsage(tab.tokens_used, tab.context_window, tab.tokens_estimated), role};
    });

    add("model", Right(kModelOrder), kModelPriority, 0, [](const TabStatus& tab) {
        return StatusValue{tab.focused ? Staged(tab, tab.model, tab.desired_model) : "", ""};
    });
    add("agent", Right(kAgentOrder), kAgentPriority, 0, [](const TabStatus& tab) {
        return StatusValue{
            tab.focused ? Staged(tab, tab.agent_preset, tab.desired_agent_preset) : "", ""};
    });

    // bypass runs every tool unasked: the one mode that must never look like
    // the others. Colored by the DISPLAYED mode, so a pending bypass is red
    // already -- it is about to be true.
    add("permission_mode", Right(kPermissionModeOrder), kPermissionModePriority, 0, [](const TabStatus& tab) {
        if (!tab.focused) return StatusValue{};
        const std::string value = PermissionModeSegmentValue(
            tab.permission_mode, tab.desired_permission_mode, !tab.session_id.empty());
        const std::string& shown =
            tab.desired_permission_mode.empty() ? tab.permission_mode : tab.desired_permission_mode;
        return StatusValue{value, !value.empty() && shown == "bypass" ? "error" : ""};
    });

    // "The agent is suspended" is exactly what the user must notice before
    // wondering why nothing is happening.
    add("paused", Right(kPausedOrder), kPausedPriority, 0, [](const TabStatus& tab) {
        return tab.focused && tab.paused ? StatusValue{"paused", "attention"} : StatusValue{};
    });

    add("activity", Left(kActivityOrder), kActivityPriority, 0, [](const TabStatus& tab) {
        if (!tab.focused) return StatusValue{};
        StatusValue value;
        // Busy states lead with the tab bar's spinner: with one tab open the
        // bar is hidden, and a static "thinking…" alone cannot say the turn
        // is still alive.
        if (tab.state == "thinking") {
            value = {client::SpinnerFrame() + " thinking…", "thinking"};
        } else if (tab.state == "working") {
            value = {client::SpinnerFrame() + " " +
                         (tab.running_tool.empty() ? "working…"
                                                   : "running " + tab.running_tool + "…"),
                     "working"};
        } else if (tab.state == "waiting") {
            value = {"waiting", "attention"};
        } else if (tab.state == "error") {
            value = {"error", "error"};
        }
        // The interrupt press is never silent: until the turn actually ends.
        if (tab.cancel_requested) value = {"cancelling…", "attention"};
        if (tab.live_view_truncated) value.text = WithHiddenRowsSuffix(value.text);
        return value;
    });
    return out;
}

}  // namespace minimal::session_status
