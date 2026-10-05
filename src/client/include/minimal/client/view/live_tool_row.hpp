#pragma once

// The pieces of a LIVE tool row (a ToolExecutionState, not a persisted
// ChatRow) that the chat view's live block and the running-tools panel both
// draw -- the two used to derive each piece separately.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/tool_status.hpp"

namespace minimal::client {

inline ToolStatus LiveToolStatus(const ToolExecutionState& tool) {
    return tool.started ? ToolStatus::Running : tool.is_error ? ToolStatus::Failed : ToolStatus::Ok;
}

inline std::string LiveToolName(const ToolExecutionState& tool) {
    return tool.tool_name.empty() ? std::string("tool") : tool.tool_name;
}

// The wire summary when one came (a background bash row's command), else the
// client's own summary of the call's input -- the usual case: summary is
// empty on the wire. One line either way: text() draws a single row whatever
// newlines the source carried.
inline std::string LiveToolSummary(const ToolExecutionState& tool) {
    return (tool.summary.has_value() && !tool.summary->empty())
               ? OneLineSummary(*tool.summary)
               : SummarizeToolInput(*tool.input);
}

// Running: the elapsed time since the server-stamped start (ticking -- the
// pump repaints while a tool runs, even a silent one). Settled: the
// server-measured duration, the number the persisted row will show. "" when
// neither is known (started_at defaults to -1: never a multi-decade elapsed).
inline std::string LiveToolTiming(const ToolExecutionState& tool) {
    if (tool.started) {
        if (tool.started_at_unix_ms.count() < 0) return {};
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        return FormatDuration(std::max<std::int64_t>(0, now_ms - tool.started_at_unix_ms.count()));
    }
    return tool.duration.count() >= 0 ? FormatDuration(tool.duration.count()) : std::string{};
}

}  // namespace minimal::client
