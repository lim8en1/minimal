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

// The walk's allocation ceilings (disclosed via WalkStats, never silent):
constexpr int kMaxWalkDepth = 1024;
constexpr int kMaxDirEntries = 10000;

// .gitignore read bound. The read used to be an unbounded
// istreambuf_iterator slurp: a generated ignore file of arbitrary size was
// materialized whole before parsing. Rules past the cap simply do not apply
// (the same degradation as one untranslatable line), never a walk failure.
constexpr std::streamoff kMaxGitignoreBytes = 1024 * 1024;

// The walk state. Three conditions stop a walk -- cancellation, timeout, and
// the callback's own "I have enough" -- and they are kept apart only so the
// stats can report WHICH one fired; every one of them means "stop and unwind
// now", so one flag drives the ITERATION. The traversal is an explicit-stack
// DFS: the recursive WalkDir put one C++ frame per directory level with no
// depth bound -- a loop of mkdir+cd through the bash tool builds trees deeper
// than any worker stack, and the overflow is a SIGSEGV no guard catches.
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

    // Reads and parses `dir`'s .gitignore, pushing it onto the stack at the
    // depth of `rel` ("" is the walk root itself, depth 0). Returns true when
    // a set was pushed -- the caller owns the matching Pop via Frame::pushed.
    // Each .gitignore is pushed exactly ONCE by this path: an earlier shape
    // also pushed the root's from WalkTree, and the duplicate (recorded at
    // depth 1 vs. this depth 0) made an anchored `/build` match `sub/build`.
    // Cancel/deadline are checked BEFORE the read, like every other
    // per-directory step -- ignore-file parsing must not be an
    // uninterruptible island inside the walk -- and the read is bounded by
    // kMaxGitignoreBytes.
    bool PushGitignore(const std::filesystem::path& dir, const std::string& rel) {
        if (options.is_cancelled && options.is_cancelled()) {
            stop = Stop::kCancelled;
            return false;
        }
        if (DeadlineExceeded(options, start)) {
            stop = Stop::kTimedOut;
            return false;
        }
        std::error_code gi_ec;
        const auto gitignore = dir / ".gitignore";
        if (!std::filesystem::is_regular_file(gitignore, gi_ec) || gi_ec) return false;
        std::ifstream in(gitignore, std::ios::binary);
        if (!in) return false;
        // Size first, then a bounded read: seek-and-tell is what keeps the
        // cap honest -- reading through the iterator first and trimming after
        // would still allocate for the whole file.
        in.seekg(0, std::ios::end);
        const auto end_pos = in.tellg();
        std::string content;
        if (end_pos > 0) {
            const auto capped = std::min<std::streamoff>(end_pos, kMaxGitignoreBytes);
            content.resize(static_cast<std::size_t>(capped));
            in.seekg(0, std::ios::beg);
            in.read(content.data(), capped);
            content.resize(static_cast<std::size_t>(in.gcount()));
        }
        const int depth =
            rel.empty() ? 0
                        : static_cast<int>(std::count(rel.begin(), rel.end(), '/')) + 1;
        ignore_stack.Push(depth, GitignoreSet::Parse(content));
        return true;
    }

    // One stack frame: a directory whose children are listed up-front, with
    // its own .gitignore already pushed.
    struct Frame {
        std::filesystem::path abs;
        std::string rel;
        bool pushed = false;  // our .gitignore sits on the stack; pop when done
        std::vector<std::filesystem::path> children;
        std::size_t next = 0;
    };

    // Lists `abs` into the frame: name-sorted (deterministic output -- a model
    // diffing two calls relies on it), cancellable and deadline-checked INSIDE
    // the listing (a million-entry directory used to materialize and sort
    // uninterruptibly), and capped at kMaxDirEntries -- the withheld remainder
    // is counted and disclosed, never silently dropped.
    Frame MakeFrame(const std::filesystem::path& abs, const std::string& rel) {
        Frame frame;
        frame.abs = abs;
        frame.rel = rel;

        // This directory's .gitignore is pushed BEFORE anything under it is
        // reported or listed against, exactly as the recursive shape ordered
        // it: a directory's rules govern its own report too. The root lands
        // here at depth 0 (PushGitignore's rel=="" branch) -- the same path
        // every other directory takes, so the depth semantics have one home.
        frame.pushed = PushGitignore(abs, rel);

        std::error_code iter_ec;
        std::filesystem::directory_iterator it(abs, iter_ec), end;
        if (iter_ec) {
            // Unreadable directory: counted, not fatal.
            stats_.skipped_errors += 1;
            return frame;
        }
        for (; it != end; it.increment(iter_ec)) {
            if (iter_ec) {
                // Stopped being readable mid-listing; keep what we have.
                stats_.skipped_errors += 1;
                break;
            }
            if (options.is_cancelled && options.is_cancelled()) {
                stop = Stop::kCancelled;
                return frame;
            }
            if (DeadlineExceeded(options, start)) {
                stop = Stop::kTimedOut;
                return frame;
            }
            if (static_cast<int>(frame.children.size()) >= kMaxDirEntries) {
                // Count the withheld remainder cheaply -- names only, no stat.
                int withheld = 1;
                for (++it; it != end; it.increment(iter_ec)) {
                    if (iter_ec) break;
                    withheld += 1;
                }
                stats_.skipped_entries += withheld;
                break;
            }
            frame.children.push_back(it->path());
        }
        std::sort(frame.children.begin(), frame.children.end());
        return frame;
    }

    void Walk(const std::filesystem::path& root_abs, const std::string& root_rel) {
        std::vector<Frame> stack;
        stack.push_back(MakeFrame(root_abs, root_rel));
        if (stopping()) return;

        while (!stack.empty()) {
            if (stopping()) return;
            Frame& frame = stack.back();
            if (frame.next >= frame.children.size()) {
                if (frame.pushed) ignore_stack.Pop();
                stack.pop_back();
                continue;
            }
            if (options.is_cancelled && options.is_cancelled()) {
                stop = Stop::kCancelled;
                return;
            }
            if (DeadlineExceeded(options, start)) {
                stop = Stop::kTimedOut;
                return;
            }

            const auto path = frame.children[frame.next++];
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
            // `.git` is skipped whatever it is: a DIRECTORY in a normal
            // checkout, but a plain FILE in a git worktree (containing
            // "gitdir: ...") -- walking it as a regular file reported
            // internal bookkeeping as a search result.
            if (filename == ".git" || filename == ".hg") continue;

            const std::string child_rel =
                frame.rel.empty() ? filename.generic_string()
                                  : frame.rel + "/" + filename.generic_string();

            // Directory prune: a directory the rules ignore is skipped
            // wholesale -- nothing below it is ever considered, which is also
            // what makes re-inclusion under an ignored directory impossible
            // (git's own behaviour).
            if (ignore_stack.IsIgnored(child_rel, is_dir)) continue;

            if (is_dir) {
                if (!Report(WalkEntry{path, child_rel, true, 0})) return;
                stats_.dirs_seen += 1;
                if (static_cast<int>(stack.size()) >= kMaxWalkDepth) {
                    stats_.skipped_deep += 1;
                    continue;
                }
                Frame child = MakeFrame(path, child_rel);
                if (stopping()) {
                    if (child.pushed) ignore_stack.Pop();
                    return;
                }
                stack.push_back(std::move(child));
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
        // The root's .gitignore is pushed by MakeFrame (via PushGitignore) at
        // depth 0, along with every other directory's -- pushing it here as
        // well recorded the same file twice, and the depth-1 duplicate made
        // anchored root patterns leak into subdirectories.
        state.Walk(root, "");
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
