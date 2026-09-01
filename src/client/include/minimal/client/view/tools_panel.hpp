#pragma once

// The running-tools panel: a CROSS-SESSION monitor (that is what distinguishes
// it from the chat view) -- one row per tool execution the client has seen,
// with live stdout. Lossy by design: the panel answers "is this progressing?",
// the capped persisted ToolResultBlock is the authoritative record
// (ARCHITECTURE.md "Running-tools panel").
//
// Bounded at the client end: only the last `stdout_buffer_lines` (default 100)
// lines of a tool's buffered output are rendered. The server-side coalescing
// that bounds the wire is E5+ server work; the client's tail keeps even an
// unbounded stream from flooding the frame.

#include <cstdint>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// Reads the LIGHT TabSnapshot.tools (track R): no full per-tab snapshots.
ftxui::Element RenderToolsPanel(const std::vector<AppState::TabSnapshot>& tabs,
                                int stdout_buffer_lines, const Theme& theme);

}  // namespace minimal::client
