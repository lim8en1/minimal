#pragma once

// session-export's pure half: the transcript as one document, and the rows
// the modal offers as start points. No host, no I/O -- what the tests drive.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::session_export {

// A whole session as read back: both lists ascending (by seq / at_seq).
struct Transcript {
    std::vector<v1::StoredMessage> messages;
    std::vector<v1::CompactionEvent> compactions;
};

// Where the export begins: the first message, or a compaction -- its summary,
// then every message after the last one it replaced (boundary_seq).
// `compaction` indexes Transcript::compactions.
struct StartPoint {
    std::optional<std::size_t> compaction;
    std::string label;  // the modal row: when, and a preview of what follows
};

struct ExportOptions {
    bool thinking = false;
    bool tool_calls = true;
    std::optional<std::size_t> compaction;  // StartPoint::compaction
    bool plain_text = false;                // .txt: the same layout, no markdown
    std::string session_id;                 // the document's title line
};

// "From start" first, then every compaction that folded something, oldest
// first. Skipped ones (a sanity guard refused them) replaced nothing, so
// there is nothing to start from.
std::vector<StartPoint> BuildStartPoints(const Transcript& transcript);

// The document. A message left with nothing to show once thinking or tool
// calls are filtered out (a tool-results-only user turn) is dropped whole,
// heading included.
std::string Render(const Transcript& transcript, const ExportOptions& options);

// "<dir>/minimal-<session id>-<YYYYmmdd-HHMMSS>.md", local time.
std::filesystem::path DefaultPath(const std::filesystem::path& dir, const std::string& session_id,
                                  std::int64_t now_unix_ms);

// Whether `path` asks for plain text (a .txt extension, any case).
bool IsPlainText(const std::filesystem::path& path);

// How many messages Render() would emit from `options.compaction` on, after
// the same filtering -- what the modal reports once the file is written.
std::size_t CountExported(const Transcript& transcript, const ExportOptions& options);

}  // namespace minimal::session_export
