#include "glob_tool.hpp"

#include <algorithm>
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

// Deterministic secondary sort: mtime descending (newest first -- "what did I
// just change" is the dominant glob intent), ties broken by rel path.
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
        const int head_limit = input.value("head_limit", settings_.glob_default_limit) > 0
                                   ? input.value("head_limit", settings_.glob_default_limit)
                                   : settings_.glob_default_limit;

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
            std::error_code ec;
            const auto target = resolved.absolute_path / pattern;
            const auto status = std::filesystem::symlink_status(target, ec);
            if (ec || status.type() == std::filesystem::file_type::not_found) {
                return ToolResult{false, "No files found"};
            }
            std::filesystem::path rel = pattern;
            return ToolResult{false, path_prefix + rel.generic_string() + "\n"};
        }

        // Glob patterns are always anchored to the search root (VS Code
        // semantics): `*.cpp` is top level only, `**/*.cpp` is everywhere.
        // A pattern that cannot be translated is a tool error, unlike in
        // gitignore where one bad line must not void a whole file.
        const auto regex_source =
            GlobToRegex(pattern, GlobFlags::kForceAnchored | GlobFlags::kBraceAlternation);
        if (!regex_source) return Error("invalid glob pattern: " + pattern);
        const re2::RE2 matcher("^" + *regex_source + "$");
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

        std::vector<MatchedPath> matches;
        const auto stats = WalkTree(
            resolved.absolute_path, options, [&](const WalkEntry& entry) {
                if (!re2::RE2::FullMatch(re2::StringPiece(entry.rel_path.data(), entry.rel_path.size()), matcher)) return true;
                MatchedPath m;
                m.rel_path = entry.rel_path;
                m.is_dir = entry.is_dir;
                if (!entry.is_dir) {
                    std::error_code ec;
                    const auto t = std::filesystem::last_write_time(entry.absolute_path, ec);
                    if (!ec) {
                        // Clock epochs differ across platforms; only the ORDER
                        // matters, and any consistent nanosecond-scale tick
                        // provides it.
                        m.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         t.time_since_epoch())
                                         .count();
                    }
                }
                matches.push_back(std::move(m));
                // The walker's hard ceiling; head_limit is applied after the
                // sort, on the NEWEST matches, so collecting to max and
                // stopping there is what makes the cap meaningful.
                return static_cast<int>(matches.size()) < settings_.glob_max_results;
            });

        if (matches.empty()) {
            // A walker stat explaining an empty result is far more honest
            // than a flat "No files found", which reads as a confident
            // negative even when the walk actually timed out, was
            // cancelled, or skipped entries it never got to enumerate --
            // exactly the footer already reported below for a NON-empty
            // result. (limit_stopped cannot fire with zero matches: the
            // walker only stops at the cap after collecting some.)
            if (stats.timed_out || stats.cancelled || stats.skipped_entries > 0 ||
                stats.skipped_deep > 0) {
                std::string out = "No files found";
                if (stats.timed_out) out += " (stopped early: timeout)";
                if (stats.cancelled) out += " (stopped early: cancelled)";
                if (stats.skipped_entries > 0) {
                    out += " (" + std::to_string(stats.skipped_entries) +
                           " director(y entries) skipped: over the per-directory cap)";
                }
                if (stats.skipped_deep > 0) {
                    out += " (" + std::to_string(stats.skipped_deep) +
                           " director(ies) skipped: deeper than the walk depth cap)";
                }
                return ToolResult{false, out};
            }
            return ToolResult{false, "No files found"};
        }

        std::sort(matches.begin(), matches.end(), [](const MatchedPath& a, const MatchedPath& b) {
            if (a.mtime_ns != b.mtime_ns) return a.mtime_ns > b.mtime_ns;
            return a.rel_path < b.rel_path;
        });

        std::string out;
        const int shown = std::min<int>(head_limit, static_cast<int>(matches.size()));
        for (int i = 0; i < shown; ++i) {
            out += path_prefix + matches[i].rel_path;
            if (matches[i].is_dir) out += '/';
            out += '\n';
        }

        // "total"/"newest" are honest only when the walk saw every match.
        // Once the walker's own result cap stopped it early (limit_stopped),
        // `matches` is a name-sorted-DFS-order PREFIX of the real match set,
        // not the whole thing sorted by time -- calling it a "total" and its
        // head "newest" overstates what was actually collected.
        const bool more = matches.size() > static_cast<std::size_t>(shown);
        if (more) {
            out += "(" + std::to_string(matches.size()) +
                   (stats.limit_stopped ? " collected; showing first " : " total; showing newest ") +
                   std::to_string(shown) + ")\n";
        }
        if (stats.limit_stopped) {
            // Fires even when head_limit >= glob_max_results (every
            // collected match is shown, `more` is false): the cap still
            // stopped the WALK, so files beyond it were never even seen,
            // let alone counted -- silence here read as a complete result.
            out += "(stopped early: result cap reached; more files may exist beyond the " +
                   std::to_string(matches.size()) + " collected)\n";
        }
        // A walk that stopped early is said so -- a partial result presented
        // as complete is the one failure mode worse than a truncation.
        if (stats.timed_out) out += "(stopped early: timeout)\n";
        if (stats.cancelled) out += "(stopped early: cancelled)\n";
        if (stats.skipped_entries > 0) {
            out += "(" + std::to_string(stats.skipped_entries) +
                   " director(y entries) skipped: over the per-directory cap)\n";
        }
        if (stats.skipped_deep > 0) {
            out += "(" + std::to_string(stats.skipped_deep) +
                   " director(ies) skipped: deeper than the walk depth cap)\n";
        }
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
