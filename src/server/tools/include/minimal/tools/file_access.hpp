#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace minimal::tools {

struct FileWindow {
    std::vector<std::string> lines;
    int start_line = 0;  // 1-based, inclusive; 0 when nothing was returned
    int end_line = 0;    // 1-based, inclusive
    int total_lines = 0;
    bool truncated = false;  // content exists past end_line
    bool is_binary = false;
    bool ok = false;
    std::string error;
};

// Reads a line window out of an already-confined absolute path.
//
// Confinement is NOT performed here. The caller resolves the path through
// path_confinement::confine first, which keeps path safety in exactly one place
// and leaves this independently testable.
//
// Memory is proportional to `limit`, not to file size -- only the requested
// window is retained -- which is what makes reading a million-line file safe.
// Time is still O(file), because `total_lines` requires reaching the end. That
// cost buys the pagination hint the model needs to ask for the next page; an
// "optimization" that stops early would have to report a total it cannot know.
//
// Binary files are refused rather than dumped into the model's context as
// mojibake: detection is a NUL byte within the first 8 KiB, which is what
// separates real text from anything with an encoded payload in practice.
//
// Lines longer than max_line_length are truncated with a visible marker rather
// than silently, so the model can tell the difference between a short line and
// a clipped one.
FileWindow read_window(const std::filesystem::path& confined_abs_path, int offset_1based, int limit,
                        int max_line_length);

}  // namespace minimal::tools
