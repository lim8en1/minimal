#include "tree_walker.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace minimal::tools_search {
namespace {

bool DeadlineExceeded(const WalkOptions& options,
                      const std::chrono::steady_clock::time_point& start) {
    if (options.timeout == std::chrono::steady_clock::duration{0}) return false;
    return std::chrono::steady_clock::now() - start >= options.timeout;
}

// The walk state. Three conditions stop a walk -- cancellation, timeout, and
// the callback's own "I have enough" -- and they are kept apart only so the
// stats can report WHICH one fired; every one of them means "stop and unwind
// now", so one flag drives the recursion.
struct State {
    const WalkOptions& options;
    const std::function<bool(const WalkEntry&)>& cb;
    std::chrono::steady_clock::time_point start;
    GitignoreStack ignore_stack;  // one set per .gitignore on the walked path

    enum class Stop { kNone, kCancelled, kTimedOut, kLimit };
    Stop stop = Stop::kNone;

    bool stopping() const { return stop != Stop::kNone; }

    WalkStats Finish() && {
        WalkStats stats = stats_;  // the counters travel with the flags
        stats.cancelled = stop == Stop::kCancelled;
        stats.timed_out = stop == Stop::kTimedOut;
        stats.limit_stopped = stop == Stop::kLimit;
        return stats;
    }

    // Returns false only when the walk must stop. Reports one entry.
    bool Report(const WalkEntry& entry) {
        if (cb(entry)) return true;
        stop = Stop::kLimit;
        return false;
    }

    void WalkDir(const std::filesystem::path& abs, const std::string& rel) {
        if (stopping()) return;

        // directory_iterator's order is unspecified, so the per-directory
        // entries are name-sorted: two walks over the same tree produce the
        // same output, which a model diffing results across calls relies on.
        std::vector<std::filesystem::path> children;
        {
            std::error_code iter_ec;
            std::filesystem::directory_iterator it(abs, iter_ec), end;
            if (iter_ec) {
                // Unreadable directory: counted, not fatal.
                stats_.skipped_errors += 1;
                return;
            }
            for (; it != end; it.increment(iter_ec)) {
                if (iter_ec) {
                    // Stopped being readable mid-listing; keep what we have.
                    stats_.skipped_errors += 1;
                    break;
                }
                children.push_back(it->path());
            }
        }
        std::sort(children.begin(), children.end());

        for (const auto& path : children) {
            if (stopping()) return;
            if (options.is_cancelled && options.is_cancelled()) {
                stop = Stop::kCancelled;
                return;
            }
            if (DeadlineExceeded(options, start)) {
                stop = Stop::kTimedOut;
                return;
            }

            std::error_code ec;
            const auto link_status = std::filesystem::symlink_status(path, ec);
            if (ec) {
                stats_.skipped_errors += 1;
                continue;
            }
            const auto type = link_status.type();
            // Symlinks are never followed -- a symlinked directory is the
            // classic traversal loop, and a symlinked file can point outside
            // the search root. Sockets/fifos/devices are not search targets.
            if (type != std::filesystem::file_type::directory &&
                type != std::filesystem::file_type::regular) {
                continue;
            }
            const bool is_dir = type == std::filesystem::file_type::directory;

            const auto filename = path.filename();
            if (is_dir && (filename == ".git" || filename == ".hg")) continue;

            const std::string child_rel = rel.empty() ? filename.generic_string()
                                                      : rel + "/" + filename.generic_string();

            // Directory prune: a directory the rules ignore is skipped
            // wholesale -- nothing below it is ever considered, which is also
            // what makes re-inclusion under an ignored directory impossible
            // (git's own behaviour).
            if (ignore_stack.IsIgnored(child_rel, is_dir)) continue;

            if (is_dir) {
                // Push before reporting or descending, so the directory's own
                // .gitignore governs everything below it, including the
                // directory's report itself.
                bool pushed = false;
                std::error_code gi_ec;
                const auto gitignore = path / ".gitignore";
                if (std::filesystem::is_regular_file(gitignore, gi_ec) && !gi_ec) {
                    std::ifstream in(gitignore, std::ios::binary);
                    if (in) {
                        std::string content((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
                        const auto depth =
                            static_cast<int>(std::count(child_rel.begin(), child_rel.end(), '/')) + 1;
                        ignore_stack.Push(depth, GitignoreSet::Parse(content));
                        pushed = true;
                    }
                }

                if (!Report(WalkEntry{path, child_rel, true, 0})) {
                    if (pushed) ignore_stack.Pop();
                    return;
                }
                stats_.dirs_seen += 1;

                WalkDir(path, child_rel);
                if (pushed) ignore_stack.Pop();
                continue;
            }

            std::uintmax_t size = 0;
            size = std::filesystem::file_size(path, ec);
            if (ec) {
                stats_.skipped_errors += 1;
                continue;
            }
            if (options.max_file_bytes > 0 &&
                static_cast<std::int64_t>(size) > options.max_file_bytes) {
                stats_.skipped_oversize += 1;
                continue;
            }

            if (!Report(WalkEntry{path, child_rel, false, size})) return;
            stats_.files_seen += 1;
        }
    }

    WalkStats stats_;
};

}  // namespace

WalkStats WalkTree(const std::filesystem::path& root, const WalkOptions& options,
                   const std::function<bool(const WalkEntry&)>& cb) {
    State state{options, cb, std::chrono::steady_clock::now(), {}, State::Stop::kNone, {}};

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(root, ec);
    if (ec || status.type() == std::filesystem::file_type::symlink) {
        // Same rule as inside the walk: the root itself is never followed
        // through a symlink. A missing/unreadable root is a skipped error.
        if (ec) state.stats_.skipped_errors += 1;
        return std::move(state).Finish();
    }

    if (status.type() == std::filesystem::file_type::directory) {
        // The root's .gitignore sits at depth 0 and governs everything below.
        std::error_code gi_ec;
        const auto gitignore = root / ".gitignore";
        if (std::filesystem::is_regular_file(gitignore, gi_ec) && !gi_ec) {
            std::ifstream in(gitignore, std::ios::binary);
            if (in) {
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                state.ignore_stack.Push(0, GitignoreSet::Parse(content));
            }
        }
        state.WalkDir(root, "");
    } else if (status.type() == std::filesystem::file_type::regular) {
        // The root is itself a file: report it and nothing else.
        const auto size = std::filesystem::file_size(root, ec);
        if (ec) {
            state.stats_.skipped_errors += 1;
        } else if (options.max_file_bytes > 0 &&
                   static_cast<std::int64_t>(size) > options.max_file_bytes) {
            state.stats_.skipped_oversize += 1;
        } else {
            state.Report(WalkEntry{root, root.filename().generic_string(), false, size});
            if (!state.stopping()) state.stats_.files_seen += 1;
        }
    }

    return std::move(state).Finish();
}

}  // namespace minimal::tools_search
