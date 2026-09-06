#include "minimal/agent/mention_expansion.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "minimal/tools/file_access.hpp"
#include "minimal/tools/path_confinement.hpp"

namespace minimal::agent {

namespace {

// Per-mention caps. A mention is a convenience window, not a substitute for the
// read tool: the truncation note says so, and the model can pull more itself.
// A file is NEVER split across blocks -- fragments would read as separate
// documents the model cannot reassemble; one bounded block with a visible note
// is the whole contract.
constexpr int kMentionMaxLines = 1000;
constexpr int kMentionMaxLineLength = 2000;
constexpr int kTreeMaxDepth = 3;  // levels BELOW the mentioned directory
constexpr std::size_t kTreeMaxEntries = 500;
// Expansion is bounded per mention AND per message: without the count cap,
// a message of N mentions reads N ~2 MiB windows into the provider prompt
// in one turn. Both ceilings leave the mention verbatim -- the model can
// still read the file itself through the gated read tool.
constexpr int kMaxMentionsPerMessage = 32;
constexpr std::size_t kMaxMentionBytesPerMessage = 8u << 20;  // 8 MiB of blocks

bool IsWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct Token {
    std::size_t begin;  // index of the '@'
    std::size_t end;    // one past the last path character
};

std::optional<Token> NextToken(std::string_view text, std::size_t from) {
    for (std::size_t i = from; i < text.size(); ++i) {
        if (text[i] != '@') continue;
        // Mid-word '@' is prose (email addresses), not a mention: the token must
        // open at the start of the text or right after whitespace.
        if (i != 0 && !IsWhitespace(text[i - 1])) continue;
        std::size_t j = i + 1;
        while (j < text.size() && !IsWhitespace(text[j])) ++j;
        if (j == i + 1) continue;  // a bare '@' is not a token
        return Token{i, j};
    }
    return std::nullopt;
}

// The path attribute carries the reference AS TYPED (minus '@'): it is what the
// user pointed at, which is the honest label whether the path was relative or
// reached outside project_root.
std::string FrameFile(const std::string& path_attr, const tools::FileWindow& window) {
    std::string out = "<file path=\"" + path_attr + "\">\n";
    for (const auto& line : window.lines) {
        out += line;
        out += '\n';
    }
    if (window.truncated) {
        out += "[truncated: showing lines " + std::to_string(window.start_line) + "-" +
               std::to_string(window.end_line) + " of " + std::to_string(window.total_lines) +
               " -- use the read tool for more]\n";
    }
    out += "</file>";
    return out;
}

// Depth- and entry-bounded listing, one relative path per line, sorted per
// directory so the output is deterministic. Returns false once the entry cap
// forced a stop. Symlinks to directories are followed (is_directory follows
// them); the depth cap is what keeps a symlink loop from recursing forever.
bool AppendTree(std::string& out, const std::filesystem::path& dir,
                const std::string& rel_prefix, int depth, std::size_t& count) {
    std::error_code ec;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        entries.push_back(entry.path());
    }
    if (ec) return true;  // unreadable subdirectory: stop this branch silently
    std::sort(entries.begin(), entries.end());

    for (const auto& path : entries) {
        if (count >= kTreeMaxEntries) {
            out += "[truncated: " + std::to_string(kTreeMaxEntries) + " entries shown]\n";
            return false;
        }
        const std::string name = path.filename().string();
        if (!name.empty() && name.front() == '.') continue;
        std::error_code type_ec;
        const bool is_dir = std::filesystem::is_directory(path, type_ec);
        out += rel_prefix + name + (is_dir ? "/" : "") + "\n";
        ++count;
        if (is_dir && depth < kTreeMaxDepth) {
            if (!AppendTree(out, path, rel_prefix + name + "/", depth + 1, count)) return false;
        }
    }
    return true;
}

std::string FrameTree(const std::string& path_attr, const std::filesystem::path& dir) {
    std::string listing;
    std::size_t count = 0;
    AppendTree(listing, dir, /*rel_prefix=*/"", /*depth=*/1, count);
    std::string out = "<file-tree path=\"" + path_attr + "\">\n";
    out += listing;
    if (!listing.empty() && listing.back() != '\n') out += '\n';
    out += "</file-tree>";
    return out;
}

}  // namespace

llm::Message ExpandMentions(std::string_view text, const std::filesystem::path& project_root,
                            const common::Logger& logger) {
    std::vector<llm::ContentBlock> blocks;
    blocks.push_back(llm::TextBlock{std::string(text)});

    int expanded = 0;
    int unresolved = 0;
    std::size_t expanded_bytes = 0;
    std::size_t search_from = 0;
    while (const auto token = NextToken(text, search_from)) {
        search_from = token->end;
        const std::string path_text(text.substr(token->begin + 1, token->end - token->begin - 1));

        if (expanded >= kMaxMentionsPerMessage ||
            expanded_bytes >= kMaxMentionBytesPerMessage) {
            ++unresolved;
            continue;
        }

        const auto resolved = tools::confine(project_root, path_text, /*must_exist=*/true);
        if (!resolved.ok) {
            ++unresolved;
            logger.debug("mention '@{}' could not be resolved: {}", path_text, resolved.error);
            continue;
        }
        // confine() merely CLASSIFIES an out-of-root path; the flag is advisory.
        // A mention must never inline material from outside the project: the
        // read tool would gate the same path, so the mention path gates here.
        if (resolved.outside_project_root) {
            ++unresolved;
            logger.debug("mention '@{}' left verbatim: resolves outside the project root",
                         path_text);
            continue;
        }

        std::error_code ec;
        if (std::filesystem::is_directory(resolved.absolute_path, ec)) {
            std::string framed = FrameTree(path_text, resolved.absolute_path);
            if (expanded_bytes + framed.size() > kMaxMentionBytesPerMessage) {
                ++unresolved;
                continue;
            }
            expanded_bytes += framed.size();
            blocks.push_back(llm::TextBlock{std::move(framed)});
            ++expanded;
            continue;
        }
        // Regular files only. A FIFO or device node would block read_window's
        // open -- a mention must never wedge the session worker.
        if (!std::filesystem::is_regular_file(resolved.absolute_path, ec) || ec) {
            ++unresolved;
            continue;
        }

        const auto window = tools::read_window(resolved.absolute_path, /*offset_1based=*/1,
                                               kMentionMaxLines, kMentionMaxLineLength);
        if (!window.ok || window.is_binary) {
            ++unresolved;
            logger.debug("mention '@{}' left verbatim: {}", path_text,
                         window.is_binary ? "binary file" : window.error);
            continue;
        }
        std::string framed = FrameFile(path_text, window);
        if (expanded_bytes + framed.size() > kMaxMentionBytesPerMessage) {
            ++unresolved;
            continue;
        }
        expanded_bytes += framed.size();
        blocks.push_back(llm::TextBlock{std::move(framed)});
        ++expanded;
    }

    if (expanded + unresolved > 0) {
        logger.info("expanded {} @-mention(s), {} left verbatim", expanded, unresolved);
    }
    return llm::Message{llm::Role::User, std::move(blocks)};
}

}  // namespace minimal::agent
