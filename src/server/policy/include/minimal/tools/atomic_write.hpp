#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::tools {

struct WriteOutcome {
    bool ok = false;
    bool created = false;  // false when an existing file was overwritten
    std::uintmax_t bytes_written = 0;
    std::string error;
};

// Writes `content` to an already-confined absolute path, atomically.
//
// Confinement is NOT performed here, for the same reason as file_access: path
// safety belongs in exactly one place.
//
// Writes to a temp file IN THE SAME DIRECTORY as the target, fsyncs, then
// renames over it. Same-directory matters and is the easy thing to get wrong:
// rename() is only atomic within a filesystem, so staging in /tmp would
// silently degrade to a non-atomic copy whenever the target lives on another
// mount. The guarantee this buys is that a reader never observes a partially
// written file, and a failed write leaves the original untouched.
//
// The parent directory must already exist. Creating it implicitly would turn a
// typo'd path into a new directory tree, which is a surprising and hard-to-undo
// side effect for a model-supplied path.
//
// `create_perms`: the mode a NEW file gets (an existing target always keeps
// its own mode -- see the .cpp). Defaults to nullopt, which means the usual
// 0644 an editor-authored file gets. A caller writing something that must
// never be group/other-readable -- a credentials file, an auth token -- must
// pass owner_read|owner_write explicitly; the default is wrong for that case
// (mkstemp's own 0600 gets WIDENED to 0644 otherwise, "to match what a normal
// file would get").
WriteOutcome write_file(const std::filesystem::path& confined_abs_path, std::string_view content,
                        std::optional<std::filesystem::perms> create_perms = std::nullopt);

}  // namespace minimal::tools
