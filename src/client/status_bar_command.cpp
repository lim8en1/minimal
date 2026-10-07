#include "minimal/client/status_bar_command.hpp"

#include <unistd.h>

#include <cstdio>
#include <map>
#include <string>

#include <ftxui/screen/terminal.hpp>

#include "client_entry.hpp"
#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "minimal/client/view/status_bar.hpp"

namespace minimal::client {

namespace {

using Side = plugins::StatusPlacement::Side;

// What a width line is measured against when stdout is not a terminal.
constexpr int kDefaultWidth = 120;

constexpr const char* kUsage =
    "usage: minimal status-bar [--width N]...\n"
    "\n"
    "Prints the status bar as the TUI composes it: the layout, a sample render\n"
    "at each --width (default: this terminal's, else 120), and a complete\n"
    "\"status_segments\" block to paste into ui.json, edit, and check again here.\n";

// A focused tab with something in every field session-status reads.
plugins::TabStatus SampleTab() {
    plugins::TabStatus tab;
    tab.focused = true;
    tab.display_name = "sample session";
    tab.tokens_used = 12345;
    tab.context_window = 200000;
    tab.model = "provider/model";
    tab.agent_preset = "build";
    tab.permission_mode = "normal";
    tab.state = "thinking";
    tab.paused = true;
    return tab;
}

std::string Line(const std::vector<StatusBarPart>& parts, int width) {
    std::string left;
    std::string right;
    bool split = false;
    for (const auto& part : parts) {
        if (part.is_split) {
            split = true;
            continue;
        }
        (split ? right : left) += part.text;
    }
    const std::size_t used = Utf8ColumnWidth(left) + Utf8ColumnWidth(right);
    const std::size_t gap = static_cast<std::size_t>(width) > used
                                ? static_cast<std::size_t>(width) - used
                                : (right.empty() ? 0 : 1);
    return left + std::string(gap, ' ') + right;
}

std::string SideName(Side side) { return side == Side::kLeft ? "left" : "right"; }

}  // namespace

int RunStatusBarCommand(const std::vector<std::string>& args) {
    std::vector<int> widths;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::fputs(kUsage, stdout);
            return app::kOk;
        }
        if (args[i] == "--width" && i + 1 < args.size()) {
            try {
                std::size_t used = 0;
                const int width = std::stoi(args[i + 1], &used);
                if (used == args[i + 1].size() && width > 0) {
                    widths.push_back(width);
                    ++i;
                    continue;
                }
            } catch (const std::exception&) {
            }
            std::fprintf(stderr, "minimal status-bar: --width wants a positive number, got '%s'\n",
                         args[i + 1].c_str());
            return app::kUsage;
        }
        std::fprintf(stderr, "minimal status-bar: unknown argument '%s'\n%s", args[i].c_str(), kUsage);
        return app::kUsage;
    }
    if (widths.empty()) {
        widths.push_back(isatty(STDOUT_FILENO) ? ftxui::Terminal::Size().dimx : kDefaultWidth);
    }

    // A plain local, never moved: TuiEnvironment's address-stability contract.
    TuiEnvironment env;
    if (!LoadClientPluginEnvironment(nullptr, env, /*announce_loads=*/false)) {
        return app::kUnavailable;
    }
    const ComposedStatusBar& bar = env.status_bar;

    // Each shown segment's sample value, priority and cap -- what the frame
    // renderer gathers each frame (frame_renderer.cpp), plugin code contained.
    const plugins::TabStatus tab = SampleTab();
    std::map<std::string, std::string> values;
    StatusLayout layout;
    const auto& registry = env.registries.status_segments;
    for (std::size_t i = 0; i < registry.All().size(); ++i) {
        const std::string& id = registry.surfaces()[i];
        if (!env.status_segment_ids.count(id)) continue;
        const auto& segment = registry.All()[i];
        std::string text;
        try {
            text = segment->Render(tab).text;
            layout.priorities[id] = segment->priority();
            if (const std::size_t cap = segment->max_columns(); cap > 0) layout.caps[id] = cap;
        } catch (...) {
        }
        // Nothing to show without a server or a session: its slot, by name.
        values[id] = text.empty() ? "{" + id + "}" : text;
    }

    std::string ids;
    for (const auto& piece : bar.format) {
        if (!piece.is_segment) {
            ids += piece.text;
        } else if (piece.text == kStatusSplitId) {
            ids += "  {>}  ";
        } else {
            ids += "{" + piece.text + "}";
        }
    }
    std::printf("layout   %s\n", ids.empty() ? "(no segment is placed)" : ids.c_str());

    for (const int width : widths) {
        const auto parts = ResolveStatusBar(bar.format, values, width, layout);
        std::string dropped;
        for (const auto& id : env.status_segment_ids) {
            bool kept = false;
            for (const auto& part : parts) kept = kept || part.segment_id == id;
            if (!kept) dropped += (dropped.empty() ? "" : ", ") + id;
        }
        std::printf("\n%-8d %s\n", width, Line(parts, width).c_str());
        if (!dropped.empty()) std::printf("         dropped: %s\n", dropped.c_str());
    }

    std::printf("\nui.json -- paste into its top-level object, edit, run this again:\n");
    std::printf("\"status_segments\": {\n");
    for (std::size_t i = 0; i < bar.segments.size(); ++i) {
        const auto& placed = bar.segments[i];
        const std::string value =
            placed.placement ? "{ \"side\": \"" + SideName(placed.placement->side) +
                                   "\", \"order\": " + std::to_string(placed.placement->order) + " }"
                             : "false";
        std::printf("  \"%s\": %s%s\n", placed.id.c_str(), value.c_str(),
                    i + 1 < bar.segments.size() ? "," : "");
    }
    std::printf("}\n");
    return app::kOk;
}

}  // namespace minimal::client
