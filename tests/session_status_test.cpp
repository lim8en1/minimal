// session-status (plugins/client/session-status): the status bar's usual
// segments, from the focused tab's state.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "session_status.hpp"

namespace plugins = minimal::client::plugins;

namespace {

// Each segment's value for one tab state, by id.
std::map<std::string, plugins::StatusValue> Render(const plugins::TabStatus& tab) {
    std::map<std::string, plugins::StatusValue> out;
    for (const auto& segment : minimal::session_status::MakeSegments()) {
        out[std::string(segment->id())] = segment->Render(tab);
    }
    return out;
}

}  // namespace

TEST_CASE("FormatTokenUsage is compact but never wrong", "[session_status]") {
    using minimal::session_status::FormatTokenUsage;
    CHECK(FormatTokenUsage(551, 262144) == "551/262k · 0%");
    CHECK(FormatTokenUsage(62000, 200000) == "62k/200k · 31%");
    CHECK(FormatTokenUsage(4250, 0) == "4.2k tok");
    CHECK(FormatTokenUsage(2000, 8000) == "2k/8k · 25%");
    CHECK(FormatTokenUsage(1'500'000, 2'000'000) == "1.5M/2M · 75%");
    // An estimate is marked as one (a compaction's after-figure, a proxy
    // that reports no usage): bytes/4 is not a measurement.
    CHECK(FormatTokenUsage(15000, 1'000'000, /*estimated=*/true) == "~15k/1M · 1%");
    CHECK(FormatTokenUsage(4250, 0, /*estimated=*/true) == "~4.2k tok");
}

TEST_CASE("PermissionModeSegmentValue shows the queued mode with the pending marker",
          "[session_status][run_mode]") {
    using minimal::session_status::PermissionModeSegmentValue;

    // Nothing queued: the applied wire truth alone.
    CHECK(PermissionModeSegmentValue("normal", "", true) == "normal");
    CHECK(PermissionModeSegmentValue("bypass", "", true) == "bypass");
    // Applied but unknown (metadata never arrived): still renders.
    CHECK(PermissionModeSegmentValue("", "", true) == "");

    // Queued shift+tab: the desired value with the marker, matching the
    // {model}/{agent} pending convention. Covers the metadata-never-arrived
    // case too (empty applied, desired set).
    CHECK(PermissionModeSegmentValue("normal", "bypass", true) == "→ bypass");
    CHECK(PermissionModeSegmentValue("", "allow-only", true) == "→ allow-only");

    // Already applied (the confirming metadata landed): the marker goes.
    CHECK(PermissionModeSegmentValue("bypass", "bypass", true) == "bypass");

    // A sessionless tab applies nothing -- no pending state to show.
    CHECK(PermissionModeSegmentValue("", "bypass", false) == "");
}

TEST_CASE("WithHiddenRowsSuffix composes the live-view cut notice, never overriding",
          "[session_status][live_segments]") {
    using minimal::session_status::WithHiddenRowsSuffix;

    // Riding a state word: appended with the same " · " literal the bar
    // renders between segments.
    CHECK(WithHiddenRowsSuffix("working") == "working · earlier rows hidden");
    CHECK(WithHiddenRowsSuffix("cancelling…") == "cancelling… · earlier rows hidden");
    // An empty activity stands alone with the notice (unknown-segment rule
    // would otherwise drop it, and a latched cut is never silent).
    CHECK(WithHiddenRowsSuffix("") == "earlier rows hidden");
    // Idempotent: a value already suffixed (a provider segment returned the
    // composed form) is not double-appended.
    CHECK(WithHiddenRowsSuffix("working · earlier rows hidden") ==
          "working · earlier rows hidden");
}

TEST_CASE("the segments render the focused tab, staged choices pending, tinted by role",
          "[session_status]") {
    plugins::TabStatus tab;
    CHECK(Render(tab).at("session").text.empty());  // no tab: nothing but the version
    CHECK_FALSE(Render(tab).at("version").text.empty());

    tab.focused = true;
    CHECK(Render(tab).at("session").text == "new session");  // sessionless, unnamed
    tab.desired_model = "main/beta";
    CHECK(Render(tab).at("model").text == "main/beta");  // sessionless: no marker

    tab.session_id = "s1";
    tab.display_name = "fix the bug";
    tab.model = "main/alpha";
    tab.agent_preset = "build";
    CHECK(Render(tab).at("model").text == "→ main/beta");  // staged, not yet confirmed
    CHECK(Render(tab).at("agent").text == "build");

    tab.tokens_used = 190000;
    tab.context_window = 200000;
    CHECK(Render(tab).at("tokens").text == "190k/200k · 95%");
    CHECK(Render(tab).at("tokens").role == "error");
    tab.tokens_used = 165000;
    CHECK(Render(tab).at("tokens").role == "attention");

    tab.permission_mode = "normal";
    tab.desired_permission_mode = "bypass";
    CHECK(Render(tab).at("permission_mode").text == "→ bypass");
    CHECK(Render(tab).at("permission_mode").role == "error");  // red before it is true

    tab.state = "working";
    tab.running_tool = "bash";
    // Led by an animated spinner frame (one glyph and a space), so only the
    // tail is stable.
    const std::string working = Render(tab).at("activity").text;
    CHECK(working.size() > std::string(" running bash…").size());
    CHECK(working.ends_with(" running bash…"));
    CHECK(Render(tab).at("activity").role == "working");
    tab.cancel_requested = true;
    tab.live_view_truncated = true;
    CHECK(Render(tab).at("activity").text == "cancelling… · earlier rows hidden");

    tab.paused = true;
    CHECK(Render(tab).at("paused").text == "paused");
    tab.read_only = true;
    CHECK(Render(tab).at("session").text == "fix the bug  (read-only)");
}

TEST_CASE("only the session name may be cut; the live state outranks the reference detail",
          "[session_status]") {
    std::map<std::string, const plugins::StatusSegment*> by_id;
    const auto segments = minimal::session_status::MakeSegments();
    for (const auto& segment : segments) by_id[std::string(segment->id())] = segment.get();
    for (const auto& [id, segment] : by_id) {
        INFO(id);
        CHECK((segment->max_columns() > 0) == (id == "session"));
    }
    CHECK(by_id.at("permission_mode")->priority() > by_id.at("activity")->priority());
    CHECK(by_id.at("activity")->priority() > by_id.at("tokens")->priority());
    CHECK(by_id.at("session")->priority() < minimal::client::kPluginStatusSegmentPriority);
    CHECK(by_id.at("version")->priority() < by_id.at("session")->priority());
}
