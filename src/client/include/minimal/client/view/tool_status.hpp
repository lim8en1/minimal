#pragma once

// One vocabulary for "how is this tool call doing", shared by every surface
// that shows one: the live chat row, the persisted transcript row, the display
// renderers, the running-tools panel and the tab bar's spinner. They used to
// disagree ("running…" / "running" / " ·" / "FAILED" / "done" / "✓" / "exit
// 1") -- the same call read differently depending on where you looked.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client {

enum class ToolStatus {
    Pending,   // a call with no result in view yet (in flight, or cut by the window)
    Running,
    Ok,
    Failed,
};

// A braille spinner frame computed from the wall clock at render time: no
// per-row frame state, and every spinner on screen ticks in step. Only asked
// for while something is actually running, when the frame pump is already
// redrawing on every delta.
inline std::string SpinnerFrame() {
    static constexpr std::array<const char*, 8> kFrames = {"⠋", "⠙", "⠹", "⠸",
                                                          "⠼", "⠴", "⠦", "⠧"};
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
    return kFrames[static_cast<std::size_t>(ms / kSpinnerFrameInterval) % kFrames.size()];
}

inline std::string ToolStatusGlyph(ToolStatus status) {
    switch (status) {
        case ToolStatus::Pending: return "○";
        case ToolStatus::Running: return SpinnerFrame();
        case ToolStatus::Ok: return "✓";
        case ToolStatus::Failed: return "✗";
    }
    return "";
}

inline ftxui::Color ToolStatusColor(ToolStatus status, const Theme& theme) {
    switch (status) {
        case ToolStatus::Pending: return MutedColor(theme);
        case ToolStatus::Running: return theme.ColorFor("working");
        case ToolStatus::Ok: return theme.ColorForOr("success", "working");
        case ToolStatus::Failed: return theme.ColorFor("error");
    }
    return ftxui::Color::Default;
}

// "✓ bash": the status glyph in its status color, then the tool name in the
// `tool` role. Every tool row's first cell, live or persisted.
inline ftxui::Element ToolHeadline(ToolStatus status, const std::string& name,
                                   const Theme& theme) {
    using namespace ftxui;
    return hbox({text(ToolStatusGlyph(status) + " ") | color(ToolStatusColor(status, theme)),
                 text(name) | color(theme.ColorForOr("tool", "syntax_function"))});
}

// "850ms" / "3.4s" / "2m 5s": one duration format, shared by the bash display
// renderer (a settled call's persisted duration_ms), the live tool row and the
// running-tools panel (a running call's elapsed time, computed from
// started_at_unix_ms). Was plugins-local (core_display_renderers.cpp); lifted
// here so all three read the same number the same way.
inline std::string FormatDuration(std::int64_t ms) {
    using std::chrono::duration_cast;
    const std::chrono::milliseconds elapsed(ms);
    if (elapsed < std::chrono::seconds(1)) return std::to_string(ms) + "ms";
    if (elapsed < std::chrono::minutes(1)) {
        using Tenths = std::chrono::duration<std::int64_t, std::deci>;
        const auto tenths = duration_cast<Tenths>(elapsed).count();
        constexpr std::int64_t kTenthsPerSecond = 10;  // NOLINT(readability-magic-numbers): deci
        return std::to_string(tenths / kTenthsPerSecond) + "." +
               std::to_string(tenths % kTenthsPerSecond) + "s";
    }
    const auto minutes = duration_cast<std::chrono::minutes>(elapsed);
    return std::to_string(minutes.count()) + "m " +
           std::to_string(duration_cast<std::chrono::seconds>(elapsed - minutes).count()) + "s";
}

// The leading disclosure glyph of a collapsible row ("▸ " collapsed, "▾ "
// expanded -- RowRenderContext::collapse_marker), or nothing for a kind a
// click never collapses. Replaces the old trailing "[+]" plus the spelled-out
// "(collapsed -- alt+c to expand)" on every collapsed row.
inline ftxui::Element Disclosure(const plugins::RowRenderContext& ctx, const Theme& theme) {
    if (ctx.collapse_marker.empty()) return ftxui::emptyElement();
    return ftxui::text(std::string(ctx.collapse_marker) + " ") | Muted(theme);
}

}  // namespace minimal::client
