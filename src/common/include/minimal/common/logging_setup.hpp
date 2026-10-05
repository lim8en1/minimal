#pragma once

// Applies settings.json's `logging.*` to the process-wide logger.
//
// The log matters beyond diagnostics: much of this codebase's error handling
// IS the log (dropped command results, a dropped subagent result, a plugin
// found twice), and an auto-spawned server has no terminal, so without the
// file sink those go nowhere.
//
// Contract: call ONCE per process, early in main, while the process is still
// single-threaded. DefaultLogger() resolves the default logger through a raw
// pointer on every call (see logger.hpp), so swapping it while another thread
// logs would free a logger under that thread. Lines logged before this call
// go to spdlog's built-in console logger. It is a free function main calls
// explicitly because the settings it reads live a layer above common/.

#include <cstdint>
#include <string>

namespace minimal::common {

struct LoggingOptions {
    std::string level = "info";
    std::string path;  // empty: console only
    std::int64_t max_size_mb = 10;
    std::int64_t max_files = 5;
    // The console sink is on by default (a server in a terminal keeps printing
    // there; the file is an addition, not a replacement). The client opts out:
    // it owns the terminal -- an out-of-band write desyncs the TUI's alternate
    // screen, and headless stdout carries JSONL -- so its logs are file-only.
    bool console = true;
};

// Returns a human-readable problem, or empty on success. Never throws and never
// fails hard: a log file that cannot be opened degrades to console-only, because
// refusing to start over logging would be a worse outcome than logging less.
std::string ConfigureLogging(const LoggingOptions& options);

}  // namespace minimal::common
