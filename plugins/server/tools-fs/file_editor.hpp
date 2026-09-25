#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::tools_fs {

struct EditOutcome {
    bool ok = false;
    std::string error;
    // What changed, echoed back so the model sees the effect. Path-free: the
    // tool appends the path as the model typed it.
    std::string summary;
    // The file's content before and after a successful edit, for the diff
    // display payload. Empty on failure.
    std::string before;
    std::string after;
};

// The mutating half of the `edit` tool, separated from the Tool subclass so the
// semantics are testable without constructing a ToolContext or going through
// the permission machinery.
//
// Both operations read the whole file (unlike read_window, which streams a
// window) because rewriting requires the full content -- hence the byte cap the
// caller enforces.
class FileEditor {
public:
    // Replaces EXACTLY one occurrence. Zero matches and multiple matches are
    // both errors, deliberately: "replace the first match" silently edits the
    // wrong line when a string is not unique, and that corruption is very hard
    // to notice in a diff after the fact.
    static EditOutcome StrReplace(const std::filesystem::path& path, std::string_view old_str,
                                   std::string_view new_str, std::int64_t max_bytes);

    // Inserts after 1-based line `insert_line`; 0 means "before the first line".
    // An insert_line past the end is an error rather than an append, since it
    // usually means the model is working from a stale view of the file.
    //
    // CRLF: in a file whose every terminator is "\r\n", both commands write
    // "\r\n" too -- insert always, str_replace when the LF old_str only
    // matches after translation. Mixed and LF files are handled byte-exact.
    static EditOutcome Insert(const std::filesystem::path& path, int insert_line,
                               std::string_view insert_text, std::int64_t max_bytes);

    // The file's current content, for diffing an overwrite -- nullopt when
    // there is nothing to diff against (missing, not a regular file, over
    // max_bytes, unreadable).
    static std::optional<std::string> ReadForDiff(const std::filesystem::path& path,
                                                  std::int64_t max_bytes);
};

}  // namespace minimal::tools_fs
