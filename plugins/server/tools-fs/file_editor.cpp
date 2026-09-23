#include "file_editor.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include "minimal/tools/atomic_write.hpp"

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
    std::ostringstream buffer;
    buffer << in.rdbuf();
    loaded.content = buffer.str();
    loaded.ok = true;
    return loaded;
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

    const std::size_t occurrences = CountOccurrences(loaded.content, old_str);
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

    const auto written = tools::write_file(path, updated);
    if (!written.ok) {
        outcome.error = written.error;
        return outcome;
    }

    outcome.ok = true;
    outcome.summary = "replaced 1 occurrence in " + path.filename().string();
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

    std::vector<std::string> lines;
    {
        std::istringstream in(loaded.content);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
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

    lines.insert(lines.begin() + insert_line, std::move(text));

    std::string updated;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        updated += lines[i];
        // Every line except the last keeps its terminator; the last only if the
        // file had one before the edit.
        if (i + 1 < lines.size() || ends_with_newline) updated += '\n';
    }

    if (auto size_error = CheckResultSize(updated, max_bytes)) {
        outcome.error = std::move(*size_error);
        return outcome;
    }

    const auto written = tools::write_file(path, updated);
    if (!written.ok) {
        outcome.error = written.error;
        return outcome;
    }

    outcome.ok = true;
    outcome.summary = "inserted after line " + std::to_string(insert_line) + " in " +
                       path.filename().string();
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
