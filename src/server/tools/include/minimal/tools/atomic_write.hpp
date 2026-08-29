#pragma once

#include <cstdint>
#include <filesystem>
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
WriteOutcome write_file(const std::filesystem::path& confined_abs_path, std::string_view content);

}  // namespace minimal::tools
