#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "ignore_rules.hpp"

namespace minimal::tools_search {

// Shared bounded traversal behind both tools. Responsibilities, in one place
// so the two tools cannot drift:
//
//   - DFS with per-directory name-sorted determinism (directory_iterator's
//     order is unspecified; a model diffing two calls would see noise)
//   - .git / .hg always skipped, gitignore rules applied via GitignoreStack
//     (pushed on the way in, popped on the way out -- the stack IS the depth)
//   - symlinks never followed, files or directories: a symlinked directory is
//     the classic traversal loop, and a symlinked file can point outside the
//     search root, which would silently search beyond where the model aimed
//   - hard bounds: max file size (files over it are skipped, not read), a
//     wall-clock timeout, and cooperative cancellation -- all three stop the
//     walk and are REPORTED in the stats, so the caller can say so in its
//     truncation footer instead of presenting a partial result as complete
//   - unreadable directories and files are skipped and counted, never fatal

struct WalkOptions {
    // Files larger than this are skipped entirely (0 = unlimited). Grep reads
    // a whole candidate file into memory to line-split it, so this is the
    // memory bound, not an optimization.
    std::int64_t max_file_bytes = 0;

    // Wall-clock budget; 0 = none (tests set this to keep them deterministic).
    std::chrono::steady_clock::duration timeout{0};

    // Polled per entry. Null is legal -- the walker never cancels then.
    std::function<bool()> is_cancelled;
};

struct WalkEntry {
    std::filesystem::path absolute_path;
    std::string rel_path;  // POSIX separators, relative to the walk root
    bool is_dir = false;
    std::uintmax_t size = 0;  // files only
};

struct WalkStats {
    int files_seen = 0;
    int dirs_seen = 0;
    int skipped_errors = 0;  // unreadable entries (permission, racy delete)
    int skipped_oversize = 0;
    bool timed_out = false;
    bool cancelled = false;      // is_cancelled() returned true
    bool limit_stopped = false;  // the callback returned false
};

// Calls `cb` for every surviving entry, pre-order (directory entry reported
// before its contents). `cb` returning false stops the walk. The walk root
// itself may be a file, in which case it is reported with rel_path equal to
// its filename.
WalkStats WalkTree(const std::filesystem::path& root, const WalkOptions& options,
                   const std::function<bool(const WalkEntry&)>& cb);

}  // namespace minimal::tools_search
