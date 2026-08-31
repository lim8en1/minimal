#pragma once

// Applies settings.json's `logging.*` to the process-wide logger.
//
// THIS EXISTED ONLY AS A COMMENT. `LoggingSettings` was fully parsed, seeded
// into bundled/settings.json, and documented in ARCHITECTURE.md as backing "a
// spdlog rotating file sink (no custom rotation code), with `path` alongside
// `session_db_path` under the platform data dir" -- and nothing anywhere read
// it. DefaultLogger() returned spdlog's console default, so there was no file
// sink and `level` was ignored, which also made every logger_.debug() in the
// tree dead output.
//
// That matters more than a missing feature, because a large share of this
// codebase's error handling IS the log: dropped CommandResults, a dropped
// subagent result, the plugin-exists-twice warning, compaction disabled for a
// session. Those are visible today only because the server happens to run in a
// terminal; the moment a client auto-spawns it (which main() already anticipates
// -- "NO DAEMON: refcounted by attached clients") they go nowhere at all.
//
// MUST BE CALLED BEFORE THE FIRST DefaultLogger(), which caches spdlog's default
// logger on first use for the process lifetime. That ordering is why this is a
// free function called explicitly from main rather than something DefaultLogger
// discovers for itself: config lives a layer above common/, and inverting the
// dependency to hide the ordering would be worse than stating it.

#include <cstdint>
#include <string>

namespace minimal::common {

struct LoggingOptions {
    std::string level = "info";
    std::string path;  // empty: console only
    std::int64_t max_size_mb = 10;
    std::int64_t max_files = 5;
};

// Returns a human-readable problem, or empty on success. Never throws and never
// fails hard: a log file that cannot be opened degrades to console-only, because
// refusing to start over logging would be a worse outcome than logging less.
std::string ConfigureLogging(const LoggingOptions& options);

}  // namespace minimal::common
