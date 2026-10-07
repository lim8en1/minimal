#include "tools_panel.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

#include "policy.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/view/tool_status.hpp"

namespace minimal::tools_panel {

namespace {

using namespace ftxui;

std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

client::ToolStatus StatusOf(const plugins::ToolActivity& tool) {
    if (tool.running) return client::ToolStatus::Running;
    return tool.failed ? client::ToolStatus::Failed : client::ToolStatus::Ok;
}

// "HH:MM:SS", local time.
std::string ClockTime(std::int64_t unix_ms) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);  // NOLINT(readability-magic-numbers): ms per s
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) return {};
    char buffer[16];  // NOLINT(readability-magic-numbers): "HH:MM:SS" and its terminator, with room
    return std::strftime(buffer, sizeof buffer, "%H:%M:%S", &local) > 0 ? buffer : "";
}

// Elapsed so far while running (a quiet long command must not read as a
// hung one), the final duration once done.
std::string Timing(const plugins::ToolActivity& tool) {
    if (tool.running) {
        if (tool.started_at_unix_ms < 0) return {};
        return client::FormatDuration(std::max<std::int64_t>(0, NowUnixMs() - tool.started_at_unix_ms));
    }
    return tool.duration_ms >= 0 ? client::FormatDuration(tool.duration_ms) : std::string();
}

}  // namespace

std::size_t TailStart(std::string_view text, int max_lines) {
    if (max_lines <= 0) return text.size();
    // A trailing partial line (live output caught mid-write) is a line too.
    int remaining = max_lines;
    if (!text.empty() && text.back() != '\n') --remaining;
    int newlines = 0;
    for (std::size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '\n') {
            ++newlines;
            if (newlines > remaining) return i;
        }
    }
    return 0;
}

Element RenderPanel(const plugins::TabActivitySnapshot& snapshot, int output_lines,
                    const client::Theme& theme) {
    Elements rows;
    bool any = false;
    for (const auto& tab : snapshot.tabs) {
        for (const auto& tool : tab.tools) {
            any = true;
            Elements line;
            line.push_back(text(" "));
            line.push_back(client::ToolHeadline(StatusOf(tool), tool.name, theme));
            // The session as a fixed-width column, so summaries line up.
            std::string session = client::TruncateUtf8Columns(tab.display_name, kSessionColumn);
            session += std::string(
                kSessionColumn - std::min(kSessionColumn, client::Utf8ColumnWidth(session)), ' ');
            line.push_back(text("  " + session) | client::Muted(theme));
            if (!tool.summary.empty()) {
                // Shrinks first, so the fields after it stay visible.
                line.push_back(text("  " + tool.summary) | client::Muted(theme) | flex_shrink);
            }
            if (tool.started_at_unix_ms >= 0) {
                if (const std::string stamp = ClockTime(tool.started_at_unix_ms); !stamp.empty()) {
                    line.push_back(text("  " + stamp) | client::Muted(theme));
                }
            }
            if (const std::string timing = Timing(tool); !timing.empty()) {
                line.push_back(text("  " + timing) | client::Muted(theme));
            }
            rows.push_back(hbox(std::move(line)));
            // The live output's tail, indented under its row, read in place.
            const std::string_view output =
                tool.output != nullptr ? std::string_view(*tool.output) : std::string_view();
            std::string_view tail = output.substr(TailStart(output, output_lines));
            // A trailing '\n' ends the last line; it does not open an empty one.
            std::size_t pos = 0;
            while (pos < tail.size()) {
                const std::size_t newline = tail.find('\n', pos);
                const std::size_t end = newline == std::string_view::npos ? tail.size() : newline;
                rows.push_back(text("   " + client::ExpandTabs(tail.substr(pos, end - pos))) |
                               color(theme.ColorFor("system_message")));
                if (newline == std::string_view::npos) break;
                pos = newline + 1;
            }
        }
    }
    if (!any) rows.push_back(text(" (no tool activity)") | client::Muted(theme));
    // The host scrolls a panel to its focused row: the newest is last.
    rows.back() = focus(std::move(rows.back()));
    return vbox(std::move(rows));
}

ToolsPanelView::ToolsPanelView(plugins::HostUi* ui, int output_lines)
    : ui_(ui), output_lines_(output_lines) {}

const plugins::ViewSpec& ToolsPanelView::spec() const {
    static const plugins::ViewSpec kSpec{kViewId, "tools", plugins::ViewPlacement::Panel,
                                         "ctrl+t"};
    return kSpec;
}

ftxui::Component ToolsPanelView::component() {
    // Rebuilt only when the activity changed, the theme did, or -- while a
    // tool runs -- a second passed (its elapsed time).
    struct Cache {
        std::uint64_t version = 0;
        std::uint64_t theme_epoch = 0;
        std::int64_t second = 0;
        Element element;
    };
    return Renderer([this, cache = std::make_shared<Cache>()] {
        const auto snapshot = ui_->Activity();
        bool running = false;
        for (const auto& tab : snapshot->tabs) {
            for (const auto& tool : tab.tools) running = running || tool.running;
        }
        const std::int64_t second = running ? NowUnixMs() / 1000 : 0;  // NOLINT(readability-magic-numbers): ms per s
        if (cache->element == nullptr || cache->version != snapshot->version ||
            cache->theme_epoch != theme_.epoch() || cache->second != second) {
            cache->element = RenderPanel(*snapshot, output_lines_, theme_);
            cache->version = snapshot->version;
            cache->theme_epoch = theme_.epoch();
            cache->second = second;
        }
        // The elapsed time moves with no event: ask for the next wall-clock
        // second -- the boundary `second` above keys on. A flat second from
        // now could land just short of it and show the next tick a whole
        // second late. (The host keeps the earliest request: cheap per frame.)
        if (running) {
            const std::int64_t into_second = NowUnixMs() % 1000;  // NOLINT(readability-magic-numbers): ms per s
            ui_->RequestRedrawAfter(std::chrono::milliseconds(1000 - into_second));  // NOLINT(readability-magic-numbers)
        }
        return cache->element;
    });
}

}  // namespace minimal::tools_panel
