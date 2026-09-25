#include "glob_tool.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <vector>

#include <re2/re2.h>

#include "glob_matcher.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "tree_walker.hpp"

namespace minimal::tools_search {
namespace {

using tools::PermissionDisclosure;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

// One result row; ordered by `newer` in GlobTool::execute.
struct MatchedPath {
    std::string rel_path;
    bool is_dir = false;
    std::int64_t mtime_ns = 0;  // 0 when the mtime cannot be read
};

class GlobTool : public tools::Tool {
public:
    explicit GlobTool(SearchSettings settings) : settings_(settings) {
        spec_.name = "glob";
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Find files by glob pattern (supports *, ?, [...], {a,b} and **). Returns paths "
            "sorted by modification time, newest first, capped at head_limit with a footer "
            "stating the total when more exist. Paths are relative to the project root, "
            "not to the `path` argument.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"pattern",
               {{"type", "string"},
                {"description",
                 "Glob relative to `path`: `src/**/*.cpp`, `*.md`, `**/*.{hpp,cpp}`"}}},
              {"path",
               {{"type", "string"},
                {"description", "Directory to search, relative to the project root "
                                "or absolute within it (default: project root)"}}},
              {"head_limit",
               {{"type", "integer"},
                {"description", "Maximum paths to return (default 200)"}}}}},
            {"required", nlohmann::json::array({"pattern"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        if (!input.contains("path") || !input.at("path").is_string()) return std::nullopt;
        return input.at("path").get<std::string>();
    }

    // The base default named only "glob" -- neither the pattern nor where it
    // searches. glob is ReadOnly (usually pre-allowed), but an out-of-workdir
    // `path` or a stricter fallback.read_only still raises this prompt, and
    // it should say what it's about to read.
    PermissionDisclosure Disclose(const nlohmann::json& input, const ToolContext&) const override {
        PermissionDisclosure disclosure;
        if (!input.contains("pattern") || !input.at("pattern").is_string()) {
            disclosure.summary = "glob: <invalid input>";
            disclosure.complete = false;
            return disclosure;
        }
        const auto pattern = input.at("pattern").get<std::string>();
        std::string path = ".";
        if (input.contains("path") && input.at("path").is_string()) {
            path = input.at("path").get<std::string>();
        }
        disclosure.summary = "glob: " + pattern + " in " + path;
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        if (!input.contains("pattern") || !input.at("pattern").is_string()) {
            return Error("missing required string field: pattern");
        }
        const auto pattern = input.at("pattern").get<std::string>();
        if (pattern.empty()) return Error("pattern must not be empty");

        // Default search root is the project root itself; the executor has
        // already classified an explicit `path` for out-of-workdir purposes.
        // The MODEL writes this input: a non-string path is an ordinary
        // mistake, reported -- never silently reinterpreted as ".".
        std::string search_root(".");
        if (input.contains("path")) {
            if (!input.at("path").is_string()) return Error("path must be a string");
            search_root = input.at("path").get<std::string>();
        }
        const auto resolved =
            tools::confine(ctx.project_root, search_root, /*must_exist=*/true);
        if (!resolved.ok) return Error(resolved.error);
        // Output paths are project-root-relative -- read's own coordinate
        // system -- so a returned path is a literal read argument whatever the
        // search root was. The walker keeps matching gitignore rules and glob
        // patterns against walk-root-relative rel_paths; the prefix joins at
        // emission only. (A file-valued `path` with a literal pattern names a
        // file, so the prefix is that file's parent directory.)
        const bool root_is_dir = std::filesystem::is_directory(resolved.absolute_path);
        const std::string path_prefix = tools::ProjectRelativePrefix(resolved, root_is_dir);

        if (input.contains("head_limit") && !input.at("head_limit").is_number_integer()) {
            return Error("head_limit must be an integer");
        }
        // head_limit is read at full width and clamped BEFORE narrowing
        // (`value(..., int)` wrapped 4294967301 to 5), then capped at
        // glob_max_results -- the ceiling on paths one call returns. A cap
        // the model asked past is disclosed; the configured default sitting
        // over the ceiling is configuration, applied silently.
        int head_limit = std::min(settings_.glob_default_limit, settings_.glob_max_results);
        bool clamped = false;
        if (input.contains("head_limit")) {
            const auto& raw = input.at("head_limit");
            const std::int64_t requested =
                raw.is_number_unsigned()
                    ? static_cast<std::int64_t>(std::min<std::uint64_t>(
                          raw.get<std::uint64_t>(),
                          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                    : raw.get<std::int64_t>();
            if (requested > settings_.glob_max_results) {
                head_limit = settings_.glob_max_results;
                clamped = true;
            } else if (requested > 0) {
                head_limit = static_cast<int>(requested);
            }
        }

        // Literal fast path: no metacharacters means at most one concrete
        // path; walking the tree to find it would be theatre.
        if (IsLiteralGlob(pattern)) {
            // CONFINEMENT BOUNDARY, not input validation: the join below puts
            // the pattern straight against the confined root, so an absolute
            // pattern would replace the root and a ".." component would
            // escape it -- an existence oracle outside project_root from a
            // ReadOnly (pre-allowed) tool with no permission prompt. The
            // non-literal path needs no such check: the walker anchors at
            // resolved.absolute_path and a pattern component can only match a
            // real filename, which ".." never is.
            const std::filesystem::path pattern_path(pattern);
            if (pattern_path.is_absolute()) {
                // The rejected pattern is deliberately not echoed: the reply
                // must carry nothing a probing model could learn from.
                return Error("pattern must be relative to the search root");
            }
            for (const auto& component : pattern_path) {
                if (component == "..") {
                    return Error("pattern must not traverse out of the search root");
                }
            }
            const std::string rel = pattern_path.generic_string();
            if (!root_is_dir) {
                // A file-valued `path`: the only thing a literal can name is
                // that file itself (joining onto a FILE names nothing).
                if (rel == resolved.absolute_path.filename().generic_string()) {
                    return ToolResult{false, path_prefix + rel + "\n"};
                }
                return ToolResult{false, "No files found"};
            }
            // Resolved through confine, like every model-supplied path: a
            // plain symlink_status on the join followed symlinked
            // INTERMEDIATE components, so `link/x` with link -> /etc
            // answered whether /etc/x exists -- the same oracle as above,
            // one indirection later. Only a target that canonicalizes inside
            // the project counts as found.
            const auto target = tools::confine(
                ctx.project_root, (resolved.absolute_path / pattern_path).string(),
                /*must_exist=*/true);
            if (!target.ok || target.outside_project_root) {
                return ToolResult{false, "No files found"};
            }
            return ToolResult{false, path_prefix + rel + "\n"};
        }

        // Glob patterns are always anchored to the search root (VS Code
        // semantics): `*.cpp` is top level only, `**/*.cpp` is everywhere.
        // A pattern that cannot be translated is a tool error, unlike in
        // gitignore where one bad line must not void a whole file.
        const auto regex_source = GlobToRegex(pattern, GlobFlags::kBraceAlternation);
        if (!regex_source) return Error("invalid glob pattern: " + pattern);
        re2::RE2::Options regex_options;
        regex_options.set_log_errors(false);  // the error goes to the model
        const re2::RE2 matcher("^" + *regex_source + "$", regex_options);
        if (!matcher.ok()) return Error("invalid glob pattern: " + pattern);

        WalkOptions options;
        options.timeout = std::chrono::steady_clock::duration{
            std::chrono::milliseconds(settings_.glob_timeout_ms)};
        options.is_cancelled = ctx.is_cancelled;
        // No max_file_bytes: the walker only stats files; content is never
        // read, so size cannot bound memory here.
        // A `path` scoped to a subdirectory must still honor the project
        // root's .gitignore, or `glob(path="packages/x")` returns hits inside
        // `packages/x/build/` that a root-level search would have excluded.
        options.boundary_root = ctx.project_root;

        // The newest `head_limit` matches, kept in a bounded heap while the
        // WHOLE tree is walked: memory is O(head_limit) however many files
        // match, the total is an exact count, and "newest" means newest of
        // everything. (The old shape stopped the walk at glob_max_results
        // matches in name order and sorted only those, so its "newest" were
        // the newest of an alphabetical prefix.) Ordering: mtime descending
        // -- "what did I just change" is the dominant glob intent -- ties by
        // rel path, so equal-mtime output is deterministic.
        const auto newer = [](const MatchedPath& a, const MatchedPath& b) {
            if (a.mtime_ns != b.mtime_ns) return a.mtime_ns > b.mtime_ns;
            return a.rel_path < b.rel_path;
        };
        std::vector<MatchedPath> kept;  // a heap under `newer`: front() is the OLDEST kept
        kept.reserve(static_cast<std::size_t>(head_limit));
        int total = 0;
        const auto stats = WalkTree(
            resolved.absolute_path, options, [&](const WalkEntry& entry) {
                if (!re2::RE2::FullMatch(re2::StringPiece(entry.rel_path.data(), entry.rel_path.size()), matcher)) return true;
                total += 1;
                // Directories carry mtime 0 (the walker stats them for type
                // only): they sort after files, as they always have.
                MatchedPath m{entry.rel_path, entry.is_dir, entry.is_dir ? 0 : entry.mtime_ns};
                if (static_cast<int>(kept.size()) < head_limit) {
                    kept.push_back(std::move(m));
                    std::push_heap(kept.begin(), kept.end(), newer);
                } else if (newer(m, kept.front())) {
                    std::pop_heap(kept.begin(), kept.end(), newer);
                    kept.back() = std::move(m);
                    std::push_heap(kept.begin(), kept.end(), newer);
                }
                return true;
            });

        // Early stops and skips, in the order the non-empty footer uses.
        std::vector<std::string> notes;
        if (stats.timed_out) notes.push_back("stopped early: timeout");
        if (stats.cancelled) notes.push_back("stopped early: cancelled");
        if (stats.skipped_entries > 0) {
            notes.push_back(std::to_string(stats.skipped_entries) +
                            " director(y entries) skipped: over the per-directory cap");
        }
        if (stats.skipped_deep > 0) {
            notes.push_back(std::to_string(stats.skipped_deep) +
                            " director(ies) skipped: deeper than the walk depth cap");
        }

        if (kept.empty()) {
            // A walker stat explaining an empty result is far more honest
            // than a flat "No files found", which reads as a confident
            // negative even when the walk actually timed out, was cancelled,
            // or skipped entries it never got to enumerate.
            std::string out = "No files found";
            for (const auto& note : notes) out += " (" + note + ")";
            return ToolResult{false, out};
        }

        std::sort(kept.begin(), kept.end(), newer);
        std::string out;
        for (const auto& match : kept) {
            out += path_prefix + match.rel_path;
            if (match.is_dir) out += '/';
            out += '\n';
        }

        // A walk that stopped early saw only part of the tree: the total is
        // a lower bound and "newest" is newest-of-what-was-seen, and the
        // footer says so -- a partial result presented as complete is the
        // one failure mode worse than a truncation.
        const bool partial = stats.timed_out || stats.cancelled;
        if (total > static_cast<int>(kept.size())) {
            out += "(" + std::string(partial ? "at least " : "") + std::to_string(total) +
                   " total; showing newest " + std::to_string(kept.size()) + ")\n";
        }
        if (clamped) {
            out += "(head_limit clipped to the configured glob max_results=" +
                   std::to_string(settings_.glob_max_results) + ")\n";
        }
        for (const auto& note : notes) out += "(" + note + ")\n";
        return ToolResult{false, out};
    }

private:
    ToolSpec spec_;
    SearchSettings settings_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeGlobTool(SearchSettings settings) {
    return std::make_unique<GlobTool>(std::move(settings));
}
}  // namespace minimal::tools_search
