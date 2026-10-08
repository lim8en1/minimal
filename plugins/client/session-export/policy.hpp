#pragma once

// session-export's own judgment calls (ARCHITECTURE.md "Policy constants").

#include <cstddef>

namespace minimal::session_export {

// Messages per GetMessages page while reading the whole transcript: large
// enough that a long session is a handful of round trips, small enough that
// one page stays well inside the call's 2 s deadline.
inline constexpr int kPageSize = 200;

// A start point's preview (first user line, summary opening): one modal row,
// enough words to recognize the conversation by.
inline constexpr std::size_t kPreviewColumns = 72;

// The default file name: "<prefix><session id>-<local YYYYmmdd-HHMMSS>.md",
// in the directory the client was started from.
inline constexpr const char* kFileNamePrefix = "minimal-";

}  // namespace minimal::session_export
