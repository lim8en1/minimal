#include "file_editor.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include "minimal/common/atomic_write.hpp"
#include "minimal/common/policy.hpp"

namespace minimal::tools_fs {
namespace {

struct LoadedFile {
    bool ok = false;
    std::string error;
    std::string content;
};

LoadedFile LoadWholeFile(const std::filesystem::path& path, std::int64_t max_bytes) {
    LoadedFile loaded;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        loaded.error = "not a regular file: " + path.string();
        return loaded;
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        loaded.error = "cannot stat: " + path.string();
        return loaded;
    }
    if (static_cast<std::int64_t>(size) > max_bytes) {
        loaded.error = "file is " + std::to_string(size) + " bytes, over the " +
                        std::to_string(max_bytes) +
                        " byte edit limit -- use a different approach for files this large";
        return loaded;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loaded.error = "cannot open: " + path.string();
        return loaded;
    }
    // The size check above is a stat taken BEFORE the open; a file still
    // being appended to (a live log) can be past the cap by the time it is
    // read. The read itself is bounded, and crossing the cap rejects the
    // whole file exactly like the stat path -- never a truncated prefix, which
    // would edit (and write back) only part of the file.
    char chunk[common::kReadChunkBytes];
    while (in) {
        in.read(chunk, sizeof(chunk));
        loaded.content.append(chunk, static_cast<std::size_t>(in.gcount()));
        if (static_cast<std::int64_t>(loaded.content.size()) > max_bytes) {
            loaded.content.clear();
            loaded.error = "file grew past the " + std::to_string(max_bytes) +
                           " byte edit limit while being read -- use a different approach for "
                           "files this large";
            return loaded;
        }
    }
    loaded.ok = true;
    return loaded;
}

// True when the file's line terminator is CRLF: at least one '\n', and EVERY
// one preceded by '\r'. A mixed file is not CRLF -- normalizing it would
// rewrite terminators on lines the caller never named.
bool UsesCrlf(std::string_view text) {
    bool any = false;
    for (std::size_t pos = text.find('\n'); pos != std::string_view::npos;
         pos = text.find('\n', pos + 1)) {
        if (pos == 0 || text[pos - 1] != '\r') return false;
        any = true;
    }
    return any;
}

// Every bare '\n' becomes "\r\n"; an existing "\r\n" is left alone.
std::string LfToCrlf(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) out += '\r';
        out += text[i];
    }
    return out;
}

std::size_t CountOccurrences(const std::string& haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    // Advance by 1, not needle.size(): str_replace's whole contract is
    // "occurs exactly once", and skipping past a just-found match undercounts
    // OVERLAPPING occurrences ("aa" in "aaa" is at offsets 0 and 1; skipping
    // to offset 2 sees only one). Undercounting here means the uniqueness
    // check passes and edits a location the caller never uniquely identified.
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

// LoadWholeFile bounds the EXISTING file against max_bytes; it says nothing
// about new_str/insert_text, which are model-controlled and unbounded. A 1 KB
// file with a 200 MB new_str sailed past the cap this checks for on the way
// in and wrote 200 MB on the way out -- the same ceiling the header describes
// as bounding "worst-case memory for the mutating edit commands" applies to
// the result, not just the input.
std::optional<std::string> CheckResultSize(const std::string& updated, std::int64_t max_bytes) {
    if (static_cast<std::int64_t>(updated.size()) > max_bytes) {
        return "the edit would produce a file of " + std::to_string(updated.size()) +
               " bytes, over the " + std::to_string(max_bytes) +
               " byte edit limit -- use a different approach for files this large";
    }
    return std::nullopt;
}

}  // namespace

EditOutcome FileEditor::StrReplace(const std::filesystem::path& path, std::string_view old_str,
                                    std::string_view new_str, std::int64_t max_bytes) {
    EditOutcome outcome;

    if (old_str.empty()) {
        outcome.error = "old_str must not be empty";
        return outcome;
    }

    const auto loaded = LoadWholeFile(path, max_bytes);
    if (!loaded.ok) {
        outcome.error = loaded.error;
        return outcome;
    }

    std::size_t occurrences = CountOccurrences(loaded.content, old_str);
    // The model writes LF; a CRLF file never contains a multi-line LF
    // old_str, and "not found" there is a dead end the model cannot see
    // past (the \r is invisible in every view it has). When -- and only
    // when -- the exact bytes miss in a consistently-CRLF file, retry with
    // both strings translated to the file's own terminator.
    std::string crlf_old;
    std::string crlf_new;
    if (occurrences == 0 && old_str.find('\n') != std::string_view::npos &&
        UsesCrlf(loaded.content)) {
        crlf_old = LfToCrlf(old_str);
        crlf_new = LfToCrlf(new_str);
        occurrences = CountOccurrences(loaded.content, crlf_old);
        if (occurrences > 0) {
            old_str = crlf_old;
            new_str = crlf_new;
        }
    }
    if (occurrences == 0) {
        outcome.error = "old_str not found in " + path.string();
        return outcome;
    }
    if (occurrences > 1) {
        // Refusing is the whole point: replacing the first of several matches
        // edits a line the caller never looked at.
        outcome.error = "old_str appears " + std::to_string(occurrences) + " times in " +
                        path.string() + "; it must match exactly once -- include more context";
        return outcome;
    }

    const std::size_t pos = loaded.content.find(old_str);
    std::string updated = loaded.content;
    updated.replace(pos, old_str.size(), new_str);

    if (auto size_error = CheckResultSize(updated, max_bytes)) {
        outcome.error = std::move(*size_error);
        return outcome;
    }

    const auto written = common::write_file(path, updated);
    if (!written.ok) {
        outcome.error = written.error;
        return outcome;
    }

    outcome.ok = true;
    outcome.summary = "replaced 1 occurrence";
    outcome.before = loaded.content;
    outcome.after = std::move(updated);
    return outcome;
}

EditOutcome FileEditor::Insert(const std::filesystem::path& path, int insert_line,
                                std::string_view insert_text, std::int64_t max_bytes) {
    EditOutcome outcome;

    if (insert_line < 0) {
        outcome.error = "insert_line must be >= 0";
        return outcome;
    }

    const auto loaded = LoadWholeFile(path, max_bytes);
    if (!loaded.ok) {
        outcome.error = loaded.error;
        return outcome;
    }

    // Whether the file ended in a newline. getline erases the distinction ("a\nb"
    // and "a\nb\n" both read as {a, b}), and rejoining below used to append '\n'
    // to every line unconditionally -- so a file not ending in one GAINED one,
    // editing bytes the caller never named. str_replace (same file, same tool)
    // never touches the terminator; the two paths must agree.
    const bool ends_with_newline =
        !loaded.content.empty() && loaded.content.back() == '\n';
    // A consistently-CRLF file is split and rejoined on "\r\n", and the
    // inserted text is translated to it: the model writes LF, and a bare LF
    // line in a CRLF file is a mixed-terminator file nobody asked for. Any
    // other file (LF, or already mixed) keeps the byte-exact LF path.
    const bool crlf = UsesCrlf(loaded.content);
    const std::string_view eol = crlf ? "\r\n" : "\n";

    std::vector<std::string> lines;
    {
        std::istringstream in(loaded.content);
        std::string line;
        while (std::getline(in, line)) {
            if (crlf && !line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    if (insert_line > static_cast<int>(lines.size())) {
        // Usually means the model is working from a stale view; appending
        // silently would hide that.
        outcome.error = "insert_line " + std::to_string(insert_line) + " is past the end of " +
                        path.string() + " (" + std::to_string(lines.size()) + " lines)";
        return outcome;
    }

    std::string text(insert_text);
    if (!text.empty() && text.back() == '\n') text.pop_back();
    if (crlf) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        text = LfToCrlf(text);
    }

    lines.insert(lines.begin() + insert_line, std::move(text));

    std::string updated;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        updated += lines[i];
        // Every line except the last keeps its terminator; the last only if the
        // file had one before the edit.
        if (i + 1 < lines.size() || ends_with_newline) updated += eol;
    }

    if (auto size_error = CheckResultSize(updated, max_bytes)) {
        outcome.error = std::move(*size_error);
        return outcome;
    }

    const auto written = common::write_file(path, updated);
    if (!written.ok) {
        outcome.error = written.error;
        return outcome;
    }

    outcome.ok = true;
    outcome.summary = "inserted after line " + std::to_string(insert_line);
    outcome.before = loaded.content;
    outcome.after = std::move(updated);
    return outcome;
}

std::optional<std::string> FileEditor::ReadForDiff(const std::filesystem::path& path,
                                                   std::int64_t max_bytes) {
    auto loaded = LoadWholeFile(path, max_bytes);
    if (!loaded.ok) return std::nullopt;
    return std::move(loaded.content);
}

}  // namespace minimal::tools_fs
