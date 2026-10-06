#include "grep_tool.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <re2/re2.h>

#include "glob_matcher.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "tree_walker.hpp"
#include "minimal/common/policy.hpp"
#include "minimal/tools/policy.hpp"

namespace minimal::tools_search {
namespace {

using tools::PermissionDisclosure;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

enum class OutputMode { kFilesWithMatches, kContent, kCount };

// NUL within the first tools::kBinarySniffBytes: the same binary heuristic
// file_access's read_window uses, so the two tools agree on what "text" means.
bool LooksBinary(std::string_view text) {
    const auto window = std::min<std::size_t>(text.size(), tools::kBinarySniffBytes);
    return text.substr(0, window).find('\0') != std::string_view::npos;
}

// Whether the pattern can drive a whole-buffer pre-filter. The pre-filter
// runs the pattern once over the entire file in (?m) mode, so `^`/`$` still
// mean line start/end; a file with no hit there has no matching line, and
// only files that DO hit pay for line splitting and per-line matching. A
// false positive is harmless (the per-line pass stays authoritative), but a
// false negative would silently drop matches, so anything whose meaning
// changes between "one line" and "the whole buffer" opts out: `\A`/`\z`
// (text start/end -- a line's start is not the buffer's) and a flag group
// that could turn multi-line back off (`(?-m)`, `(?i-m)`, ...).
bool PrefilterSafe(std::string_view pattern) {
    if (pattern.find("\\A") != std::string_view::npos) return false;
    if (pattern.find("\\z") != std::string_view::npos) return false;
    for (std::size_t pos = pattern.find("(?"); pos != std::string_view::npos;
         pos = pattern.find("(?", pos + 2)) {
        std::size_t i = pos + 2;
        while (i < pattern.size() && std::isalpha(static_cast<unsigned char>(pattern[i]))) ++i;
        if (i < pattern.size() && pattern[i] == '-') return false;
    }
    return true;
}

// RE2 logs every failed compile to stderr by default. Model-authored
// patterns fail routinely, and the error already reaches the model as the
// tool result -- the server log is not the place for it.
re2::RE2::Options QuietOptions() {
    re2::RE2::Options options;
    options.set_log_errors(false);
    return options;
}

// A model-authored integer field, read at full width and clamped into
// [lo, hi] BEFORE it is narrowed: `get<int>()` on 4294967301 wraps to 5 -- a
// head_limit the model never asked for, with no error to tell it so. The
// caller has already checked is_number_integer().
int ClampedInt(const nlohmann::json& value, int lo, int hi) {
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        return raw > static_cast<std::uint64_t>(hi) ? hi : std::max(lo, static_cast<int>(raw));
    }
    const auto raw = value.get<std::int64_t>();
    return static_cast<int>(std::clamp<std::int64_t>(raw, lo, hi));
}

// One merged block of consecutive output: a match plus its context window,
// with overlaps between neighbouring matches merged. Emitted or skipped as a
// unit -- context that belongs to a match is never split from it.
struct MatchBlock {
    int start_line = 0;  // inclusive, 0-based
    int end_line = 0;    // inclusive
    std::vector<int> match_lines;
};

std::vector<MatchBlock> BuildBlocks(const std::vector<int>& match_lines, int line_count,
                                     int context_before, int context_after) {
    std::vector<MatchBlock> blocks;
    for (const int match : match_lines) {
        if (!blocks.empty()) {
            auto& prev = blocks.back();
            // Merge when this match's window overlaps the previous block or
            // starts on the line right after it -- contiguous output reads
            // as one hunk. Any real gap (even one line) is a separate block
            // with a `--` separator, as in ripgrep.
            if (match - context_before <= prev.end_line + 1) {
                prev.end_line = std::min(line_count - 1, match + context_after);
                prev.match_lines.push_back(match);
                continue;
            }
        }
        blocks.push_back(MatchBlock{std::max(0, match - context_before),
                                    std::min(line_count - 1, match + context_after),
                                    {match}});
    }
    return blocks;
}

// Per-invocation accumulator. Everything the per-file scan needs to share
// lives here, so ScanFile stays a function of (file, run) rather than a
// ten-parameter friendship.
struct GrepRun {
    GrepRun(OutputMode mode, int context_before, int context_after, int head_limit, int offset,
            ToolContext& ctx, std::string path_prefix)
        : mode(mode),
          context_before(context_before),
          context_after(context_after),
          head_limit(head_limit),
          offset(offset),
          ctx(ctx),
          path_prefix(std::move(path_prefix)) {}

    OutputMode mode;
    int context_before;
    int context_after;
    int head_limit;   // budget over MATCH units: files, or matching lines
    int offset;       // matches withheld from the top
    ToolContext& ctx;
    // Prepended to every emitted path: project-root-relative -- read's own
    // coordinate system -- so a result line is a literal read argument
    // whatever the search root was. "" when the search root IS the project
    // root (or a file directly in it).
    std::string path_prefix;

    int total_matches = 0;  // every match seen in scanned files (files count 1x in
                            // files modes, matching-lines count 1x in content mode)
    int files_with_matches = 0;
    int emitted = 0;         // matches actually shown
    int offset_consumed = 0; // matches skipped to satisfy the offset
    bool budget_exhausted = false;
    // Files the SCAN (not the walker) left unsearched -- disclosed alongside
    // the walker's own counts, never silently folded into "no matches".
    int skipped_oversize = 0;   // grew past max_file_bytes between stat and read
    int skipped_by_rules = 0;   // withheld by the operator's path rules
    int skipped_unreadable = 0; // a read error mid-file
    std::string out;

    bool BudgetLeft() const { return emitted < head_limit; }

    // Appends one line of output, streaming it to the running-tools panel.
    void Append(std::string_view line) {
        out.append(line);
        if (ctx.emit_output) ctx.emit_output(line);
    }

    // Returns false when the walk must stop (budget ran out).
    bool ReportFile(const std::string& rel_path, int match_count) {
        total_matches += (mode == OutputMode::kContent) ? match_count : 1;
        files_with_matches += 1;
        if (mode == OutputMode::kContent) return true;  // content is emitted per block

        if (offset_consumed < offset) {
            offset_consumed += 1;
            return true;
        }
        if (!BudgetLeft()) {
            budget_exhausted = true;
            return false;
        }
        Append(path_prefix + rel_path + (mode == OutputMode::kCount
                               ? ":" + std::to_string(match_count)
                               : "") + "\n");
        emitted += 1;
        return true;
    }

    // content mode: emit blocks until the budget stops us. Returns false to
    // stop the walk.
    bool ReportBlocks(const std::string& rel_path, const std::vector<std::string_view>& lines,
                      const std::vector<MatchBlock>& blocks, int max_line_length) {
        int remaining_offset = offset - offset_consumed;
        bool emitted_any = false;
        for (const auto& block : blocks) {
            const int block_matches = static_cast<int>(block.match_lines.size());
            if (remaining_offset >= block_matches) {
                // The whole block sits behind the offset: consume its matches
                // and emit nothing -- partial blocks would strand context.
                remaining_offset -= block_matches;
                offset_consumed += block_matches;
                continue;
            }
            const int shown_matches = block_matches - std::max(0, remaining_offset);
            if (emitted + shown_matches > head_limit) {
                if (emitted > 0) {
                    // A block is atomic: emitting a fraction of it would
                    // present context with no match, so the budget stops
                    // the walk here.
                    budget_exhausted = true;
                    return false;
                }
                // Nothing shown yet at ALL: refusing this one oversized
                // block would report "no matches" (or re-hit the same
                // block forever via the offset=N hint) on a search that
                // found real matches, with no way to ever page past it.
                // Force it through -- the walk still stops right after
                // (below): exceeding head_limit by one block is the
                // least-bad option, not a license to keep going.
            }
            // A partial in-block skip must count toward offset_consumed too
            // -- otherwise the same remaining offset is applied AGAIN to
            // later blocks/files and silently drops matches that were never
            // shown, plus corrupts the "showing X of Y" footer arithmetic.
            offset_consumed += std::max(0, remaining_offset);
            remaining_offset = 0;
            if (emitted_any) Append("--\n");  // ripgrep's group separator
            for (int line = block.start_line; line <= block.end_line; ++line) {
                const bool is_match =
                    std::binary_search(block.match_lines.begin(), block.match_lines.end(), line);
                std::string rendered =
                    path_prefix + rel_path + (is_match ? ":" : "-") + std::to_string(line + 1) + ":";
                auto text = lines[line];
                if (static_cast<int>(text.size()) > max_line_length) {
                    text = text.substr(0, max_line_length);
                    rendered += std::string(text) + " [...truncated]\n";
                } else {
                    rendered += std::string(text) + "\n";
                }
                Append(rendered);
            }
            emitted += shown_matches;
            emitted_any = true;
            if (emitted > head_limit) {
                // Only reachable via the forced-oversized-block case above
                // (the normal over-budget path already returned false
                // before emitting anything for this block).
                budget_exhausted = true;
                return false;
            }
        }
        return true;
    }
};

class GrepTool : public tools::Tool {
public:
    explicit GrepTool(SearchSettings settings) : settings_(std::move(settings)) {
        spec_.name = "grep";
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Search file contents with a regex (RE2 syntax: no backreferences or "
            "lookarounds). Respects .gitignore and skips binary files. output_mode: "
            "files_with_matches (default), content (path:line:text with optional context) or "
            "count (path:matches). Paths are relative to the project root, not to the `path` "
            "argument. Results are capped; the footer states what was withheld.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"}, {"description", "Regular expression to match"}}},
              {"path",
               {{"type", "string"},
                {"description", "File or directory to search, relative to the project root "
                                "or absolute within it (default: project root)"}}},
              {"glob",
               {{"type", "string"},
                {"description",
                 "Only search files matching this glob, e.g. `*.py` or `src/**/*.{h,cpp}`. "
                 "Relative to `path` (not the project root): with path `src`, use "
                 "`**/*.cpp`, not `src/**/*.cpp`"}}},
              {"output_mode",
               {{"type", "string"},
                {"enum", nlohmann::json::array({"files_with_matches", "content", "count"})}}},
              {"case_insensitive", {{"type", "boolean"}, {"description", "Default false"}}},
              {"context_before",
               {{"type", "integer"},
                {"description", "content mode only: context lines above each match"}}},
              {"context_after",
               {{"type", "integer"},
                {"description", "content mode only: context lines below each match"}}},
              {"head_limit",
               {{"type", "integer"},
                {"description", "Maximum files (files_with_matches, count) or matching lines "
                                "(content) to report; default 250"}}},
              {"offset",
               {{"type", "integer"}, {"description", "Skip this many matches first"}}}}},
            {"required", nlohmann::json::array({"pattern"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    // grep reads file CONTENT: `grep(path=".env", pattern=".")` dumped a
    // dotenv file with no prompt while read/write/edit denied it. A search
    // ROOTED at a dotenv file hits the default deny like read does, and a
    // directory walk withholds dotenv files through ctx.path_permitted -- the
    // same default rules, applied per walked file.
    bool default_dotenv_protected() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        if (!input.contains("path") || !input.at("path").is_string()) return std::nullopt;
        return input.at("path").get<std::string>();
    }

    // See GlobTool::Disclose: names the pattern and the search root instead
    // of just the word "grep".
    PermissionDisclosure Disclose(const nlohmann::json& input, const ToolContext&) const override {
        PermissionDisclosure disclosure;
        if (!input.contains("pattern") || !input.at("pattern").is_string()) {
            disclosure.summary = "grep: <invalid input>";
            disclosure.complete = false;
            return disclosure;
        }
        const auto pattern = input.at("pattern").get<std::string>();
        std::string path = ".";
        if (input.contains("path") && input.at("path").is_string()) {
            path = input.at("path").get<std::string>();
        }
        disclosure.summary = "grep: " + pattern + " in " + path;
        return disclosure;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        // --- input validation. The MODEL writes this input, so every field is
        // checked explicitly, never value()d: a wrong-typed field is an
        // ordinary mistake, reported as an error result.
        if (!input.contains("pattern") || !input.at("pattern").is_string()) {
            return Error("missing required string field: pattern");
        }
        const auto pattern = input.at("pattern").get<std::string>();
        if (pattern.empty()) return Error("pattern must not be empty");

        OutputMode mode = OutputMode::kFilesWithMatches;
        if (input.contains("output_mode")) {
            if (!input.at("output_mode").is_string()) {
                return Error("output_mode must be a string");
            }
            const auto m = input.at("output_mode").get<std::string>();
            if (m == "files_with_matches") mode = OutputMode::kFilesWithMatches;
            else if (m == "content") mode = OutputMode::kContent;
            else if (m == "count") mode = OutputMode::kCount;
            else return Error("output_mode must be files_with_matches, content or count");
        }

        if (input.contains("case_insensitive") && !input.at("case_insensitive").is_boolean()) {
            return Error("case_insensitive must be a boolean");
        }
        const bool case_insensitive = input.value("case_insensitive", false);

        int context_before = 0;
        int context_after = 0;
        if (input.contains("context_before")) {
            if (!input.at("context_before").is_number_integer()) {
                return Error("context_before must be an integer");
            }
            context_before =
                ClampedInt(input.at("context_before"), 0, settings_.grep_max_context_lines);
        }
        if (input.contains("context_after")) {
            if (!input.at("context_after").is_number_integer()) {
                return Error("context_after must be an integer");
            }
            context_after =
                ClampedInt(input.at("context_after"), 0, settings_.grep_max_context_lines);
        }
        if ((context_before > 0 || context_after > 0) && mode != OutputMode::kContent) {
            return Error("context_before/context_after require output_mode: content");
        }

        if (input.contains("head_limit") && !input.at("head_limit").is_number_integer()) {
            return Error("head_limit must be an integer");
        }
        // The model's head_limit is a REQUEST, not the ceiling: grep_max_results
        // is the configured absolute bound ("the absolute ceiling on collected
        // results", search_settings.hpp). It was parsed, tested -- and read by
        // nothing, so `head_limit: 2000000000` in content mode accumulated
        // unboundedly, bounded only by the walk timeout.
        int head_limit = settings_.grep_default_head_limit;
        bool clamped = false;
        if (input.contains("head_limit")) {
            const int requested =
                ClampedInt(input.at("head_limit"), 0, std::numeric_limits<int>::max());
            if (requested > 0) head_limit = requested;
        }
        if (head_limit > settings_.grep_max_results) {
            head_limit = settings_.grep_max_results;
            clamped = true;
        }

        if (input.contains("offset") && !input.at("offset").is_number_integer()) {
            return Error("offset must be an integer");
        }
        const int offset = input.contains("offset")
                               ? ClampedInt(input.at("offset"), 0, std::numeric_limits<int>::max())
                               : 0;

        re2::RE2::Options regex_options = QuietOptions();
        // RE2 spells the knob as case_sensitive; false is what the model's
        // `case_insensitive: true` asks for.
        regex_options.set_case_sensitive(!case_insensitive);
        const re2::RE2 matcher(pattern, regex_options);
        if (!matcher.ok()) {
            return Error("invalid regex: " + std::string(matcher.error()));
        }
        // The whole-buffer pre-filter (see PrefilterSafe). Absent when the
        // pattern opts out or the (?m) form fails to compile for any reason;
        // every file then takes the per-line path, which is always correct.
        std::optional<re2::RE2> prefilter;
        if (PrefilterSafe(pattern)) {
            prefilter.emplace("(?m)(?:" + pattern + ")", regex_options);
            if (!prefilter->ok()) prefilter.reset();
        }

        // The optional glob filter. Same anchoring rule as gitignore: a glob
        // without `/` matches a basename at any depth; one with `/` is
        // anchored to the search root.
        std::optional<re2::RE2> file_filter;
        if (input.contains("glob")) {
            if (!input.at("glob").is_string()) return Error("glob must be a string");
            const auto glob = input.at("glob").get<std::string>();
            if (glob.empty()) return Error("glob must not be empty");
            const bool anchored = glob.find('/') != std::string::npos;
            // kBraceAlternation: the schema advertises `src/**/*.{h,cpp}`, and
            // gitignore deliberately does NOT get this flag (braces are literal
            // there) -- the two callers must not silently agree to differ.
            const auto source = GlobToRegex(glob, GlobFlags::kBraceAlternation);
            if (!source) return Error("invalid glob: " + glob);
            file_filter.emplace(anchored ? "^" + *source + "$" : "(?:.*/)?" + *source + "$",
                                QuietOptions());
            if (!file_filter->ok()) return Error("invalid glob: " + glob);
        }

        const std::string search_root = [&] {
            if (!input.contains("path")) return std::string(".");
            // The MODEL writes this input: a non-string path is an ordinary
            // mistake, reported -- never silently reinterpreted as ".".
            if (!input.at("path").is_string()) return std::string();
            return input.at("path").get<std::string>();
        }();
        if (search_root.empty()) return Error("path must be a string");
        const auto resolved = tools::confine(ctx.project_root, search_root, /*must_exist=*/true);
        if (!resolved.ok) return Error(resolved.error);

        // The search root may be a file (WalkTree reports it under its bare
        // filename), so the prefix is the file's PARENT directory in that
        // case -- ProjectRelativePrefix's file branch.
        const bool root_is_dir = std::filesystem::is_directory(resolved.absolute_path);
        GrepRun run(mode, context_before, context_after, head_limit, offset, ctx,
                    tools::ProjectRelativePrefix(resolved, root_is_dir));

        WalkOptions options;
        options.timeout = std::chrono::steady_clock::duration{
            std::chrono::milliseconds(settings_.grep_timeout_ms)};
        options.max_file_bytes = settings_.grep_max_file_bytes;
        options.is_cancelled = ctx.is_cancelled;
        // A `path` scoped to a subdirectory must still honor the project
        // root's .gitignore, or `grep(path="src")` returns hits inside
        // `src/build/` that a root-level search would have excluded.
        options.boundary_root = ctx.project_root;

        const auto stats = WalkTree(
            resolved.absolute_path, options, [&](const WalkEntry& entry) {
                if (entry.is_dir) return true;  // reported for traversal only
                if (file_filter && !re2::RE2::FullMatch(re2::StringPiece(entry.rel_path.data(), entry.rel_path.size()), *file_filter)) {
                    return true;
                }
                // The permission gate resolved the call's PATH -- the walk
                // root -- so the operator's per-path rules (the default
                // dotenv deny, a `"**/secrets/**": deny`, an ask pattern) are
                // applied to each walked file here. Counted, so the skip is
                // disclosed rather than read as "no matches".
                if (ctx.path_permitted && !ctx.path_permitted(entry.absolute_path)) {
                    run.skipped_by_rules += 1;
                    return true;
                }
                return ScanFile(entry, matcher, prefilter ? &*prefilter : nullptr, run);
            });
        const int skipped_oversize = stats.skipped_oversize + run.skipped_oversize;
        const int skipped_errors = stats.skipped_errors + run.skipped_unreadable;

        // Disclosures (skips, early stops) are emitted even when nothing
        // matched -- "no matches" presented without the reason it saw only
        // part of the tree would be a false negative.
        if (run.out.empty()) {
            // "No matches found" is only honest when the tree had nothing:
            // an offset past the match count consumes matches silently, and
            // reporting that as "no matches" reads as a complete negative
            // over a tree that DID match.
            std::string headline =
                run.total_matches > 0
                    ? "no matches at offset " + std::to_string(offset) + " (" +
                          std::to_string(run.total_matches) +
                          " total -- lower the offset to see them)"
                    : "No matches found";
            const bool anything_to_disclose =
                skipped_oversize > 0 || stats.skipped_entries > 0 || stats.skipped_deep > 0 ||
                skipped_errors > 0 || run.skipped_by_rules > 0 || stats.timed_out ||
                stats.cancelled;
            if (!anything_to_disclose) return ToolResult{false, std::move(headline)};
            // The skip footers below say why the negative is only partial; the
            // headline still says what was (not) found, so the result never
            // reads as footers with no answer.
            run.Append(headline + "\n");
        }
        // Entries the walker could not read are disclosed even when something
        // DID match -- and when nothing did, they keep the empty result from
        // reading as a complete answer: "no matches" over a tree part of which
        // was never read would be a false negative stated as fact.

        // --- footer. The model has no structured channel, so the truncation
        // affordance must live in the text -- read's footer, same rule. A
        // walk that stopped before the whole tree was seen leaves the totals
        // a LOWER BOUND, and the footer says so.
        const bool walk_complete =
            !stats.timed_out && !stats.cancelled && !run.budget_exhausted;
        const bool partial = !walk_complete;
        if (mode == OutputMode::kContent) {
            if (partial || run.offset_consumed > 0 ||
                run.total_matches > run.emitted + run.offset_consumed) {
                out_footer(run, partial);
            }
        } else {
            if (partial || run.files_with_matches > run.emitted + run.offset_consumed) {
                out_footer(run, partial);
            }
        }
        if (stats.timed_out) run.Append("(stopped early: timeout)\n");
        if (stats.cancelled) run.Append("(stopped early: cancelled)\n");
        if (skipped_oversize > 0) {
            run.Append("(" + std::to_string(skipped_oversize) +
                       " file(s) skipped: larger than " +
                       std::to_string(settings_.grep_max_file_bytes) + " bytes)\n");
        }
        if (run.skipped_by_rules > 0) {
            run.Append("(" + std::to_string(run.skipped_by_rules) +
                       " file(s) withheld by permission rules; grep one directly by path to be "
                       "asked)\n");
        }
        if (clamped) {
            run.Append("(head_limit clipped to the configured grep_max_results=" +
                       std::to_string(settings_.grep_max_results) + ")\n");
        }
        if (stats.skipped_entries > 0) {
            run.Append("(" + std::to_string(stats.skipped_entries) +
                       " director(y entries) skipped: over the per-directory cap)\n");
        }
        if (stats.skipped_deep > 0) {
            run.Append("(" + std::to_string(stats.skipped_deep) +
                       " director(ies) skipped: deeper than the walk depth cap)\n");
        }
        if (skipped_errors > 0) {
            run.Append("(" + std::to_string(skipped_errors) +
                       " file(s)/director(ies) could not be read and were skipped)\n");
        }
        return ToolResult{false, std::move(run.out)};
    }

private:
    void out_footer(GrepRun& run, bool partial) {
        const int shown_total = run.emitted + run.offset_consumed;
        std::string line = "(showing " + std::to_string(run.emitted) + " of " +
                           (partial ? "at least " : "") +
                           std::to_string(mode_total(run)) + " " +
                           (run.mode == OutputMode::kContent ? "matching lines"
                                                             : "files with matches");
        if (run.budget_exhausted && run.emitted > 0) {
            line += "; re-run with offset=" + std::to_string(shown_total) + " for more";
        }
        line += ")\n";
        run.Append(line);
    }

    int mode_total(const GrepRun& run) const {
        return run.mode == OutputMode::kContent ? run.total_matches : run.files_with_matches;
    }

    // Reads one file (whole, into memory, bounded by max_file_bytes), matches
    // per line, and reports into `run`. Returns false to stop the walk.
    bool ScanFile(const WalkEntry& entry, const re2::RE2& matcher, const re2::RE2* prefilter,
                  GrepRun& run) const {
        // O_NOFOLLOW closes the walk-vs-open race: the walker vetoes symlinks
        // with symlink_status, but a symlink swapped in between that check and
        // this open would make a plain ifstream follow it outside the search
        // root. The veto is re-checked AT open time; ELOOP reads as "it is a
        // symlink now" and the file is skipped like any other unreadable
        // candidate.
        const int fd = ::open(entry.absolute_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return true;  // unreadable mid-walk / symlinked: skip quietly
        // The walker's size check is a stat taken before this open; a file
        // still being appended to (a live log) can be past the cap by now.
        // The read is bounded too, and crossing the cap skips the WHOLE file
        // (disclosed as oversize) -- a truncated prefix searched as if it
        // were the file would hide its tail without saying so. The binary
        // sniff runs as soon as its window is in, so a large binary is not
        // read in full just to be discarded.
        const std::int64_t cap = settings_.grep_max_file_bytes;
        std::string text;
        char chunk[common::kReadChunkBytes];
        bool sniffed = false;
        for (;;) {
            const ssize_t got = ::read(fd, chunk, sizeof(chunk));
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) {
                ::close(fd);
                run.skipped_unreadable += 1;
                return true;
            }
            if (got == 0) break;
            text.append(chunk, static_cast<std::size_t>(got));
            if (!sniffed && text.size() >= tools::kBinarySniffBytes) {
                sniffed = true;
                if (LooksBinary(text)) {
                    ::close(fd);
                    return true;
                }
            }
            if (cap > 0 && static_cast<std::int64_t>(text.size()) > cap) {
                ::close(fd);
                run.skipped_oversize += 1;
                return true;
            }
        }
        ::close(fd);
        if (!sniffed && LooksBinary(text)) return true;

        if (prefilter &&
            !re2::RE2::PartialMatch(re2::StringPiece(text.data(), text.size()), *prefilter)) {
            return true;  // no line can match: skip the split and per-line pass
        }

        std::vector<std::string_view> lines;
        {
            // Views into `text`'s buffer -- substr() would hand back a
            // temporary std::string the view would dangle on.
            std::size_t start = 0;
            while (start < text.size()) {
                const auto nl = text.find('\n', start);
                if (nl == std::string_view::npos) {
                    lines.emplace_back(text.data() + start, text.size() - start);
                    break;
                }
                lines.emplace_back(text.data() + start, nl - start);
                start = nl + 1;
            }
        }

        // files_with_matches needs one hit per file, not all of them: the
        // count it reports is the file, so the scan stops at the first match.
        const bool first_hit_only = run.mode == OutputMode::kFilesWithMatches;
        std::vector<int> match_lines;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (re2::RE2::PartialMatch(re2::StringPiece(lines[i].data(), lines[i].size()), matcher)) {
                match_lines.push_back(i);
                if (first_hit_only) break;
            }
        }
        if (match_lines.empty()) return true;

        if (run.mode == OutputMode::kContent) {
            if (!run.ReportFile(entry.rel_path, static_cast<int>(match_lines.size()))) {
                return false;  // updates the totals; content itself emits below
            }
            const auto blocks =
                BuildBlocks(match_lines, static_cast<int>(lines.size()),
                            run.context_before, run.context_after);
            return run.ReportBlocks(entry.rel_path, lines, blocks,
                                    settings_.grep_max_line_length);
        }
        return run.ReportFile(entry.rel_path, static_cast<int>(match_lines.size()));
    }

    ToolSpec spec_;
    SearchSettings settings_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeGrepTool(SearchSettings settings) {
    return std::make_unique<GrepTool>(std::move(settings));
}

}  // namespace minimal::tools_search
