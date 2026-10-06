#include "minimal/tools/file_access.hpp"
#include "minimal/common/units.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace minimal::tools {
namespace {

constexpr std::size_t kBinarySniffBytes = 8192;
constexpr const char* kTruncationMarker = "... [line truncated]";
constexpr const char* kNulMarker =
    "... [line truncated at a NUL byte; file may be binary past the first 8 KiB]";

// The ceiling when no max_line_length is configured. There has to be one: the
// promise this function makes is that memory tracks `limit`, and "no configured
// truncation" is not the same as "willing to allocate a whole file for one
// line". Generous enough that no real source line reaches it.
constexpr std::size_t kHardLineCap = common::kMiB;

bool LooksBinary(std::ifstream& in) {
    std::array<char, kBinarySniffBytes> buffer{};
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = static_cast<std::size_t>(in.gcount());
    in.clear();
    in.seekg(0);
    return std::find(buffer.begin(), buffer.begin() + read, '\0') != buffer.begin() + read;
}

}  // namespace

FileWindow read_window(const std::filesystem::path& confined_abs_path, int offset_1based, int limit,
                        int max_line_length) {
    FileWindow window;

    if (offset_1based < 1) offset_1based = 1;
    if (limit < 0) limit = 0;

    std::error_code ec;
    if (std::filesystem::is_directory(confined_abs_path, ec)) {
        window.error = "is a directory: " + confined_abs_path.string();
        return window;
    }
    // Everything here runs on the session worker, and std::ifstream on a FIFO
    // (or a device) never reaches EOF: the worker parks forever on one tool
    // call. is_regular_file follows symlinks, so a link pointing at a real
    // file still reads; anything the kernel serves without a size -- FIFO,
    // socket, device node -- is refused up front.
    if (!std::filesystem::is_regular_file(confined_abs_path, ec)) {
        window.error = "not a regular file: " + confined_abs_path.string();
        return window;
    }

    std::ifstream in(confined_abs_path, std::ios::binary);
    if (!in) {
        window.error = "cannot open: " + confined_abs_path.string();
        return window;
    }

    if (LooksBinary(in)) {
        window.is_binary = true;
        window.error = "binary file (NUL byte in first 8 KiB): " + confined_abs_path.string();
        return window;
    }

    // offset + limit in int64 first: both come from model-written JSON, and
    // `offset_1based + limit - 1` overflowed for values near INT_MAX. Clamped to
    // INT_MAX, which means "to the end of the file" here anyway.
    const std::int64_t raw_end =
        static_cast<std::int64_t>(offset_1based) + static_cast<std::int64_t>(limit) - 1;
    const int window_end =
        (limit == 0) ? 0
                     : static_cast<int>(std::min<std::int64_t>(
                           raw_end, std::numeric_limits<int>::max()));

    // BOUNDED PER LINE, which is what makes the documented promise -- "memory
    // proportional to `limit`, not file size" -- actually true. std::getline
    // materializes the WHOLE line before anything truncates it, so a 500 MB
    // minified file with no newline (and no NUL, so the binary probe passes) was
    // a 500 MB allocation to display 2000 characters. Reading into a bounded
    // buffer and discarding the remainder keeps the peak at the buffer size.
    const std::size_t keep =
        max_line_length > 0 ? static_cast<std::size_t>(max_line_length) : kHardLineCap;
    std::vector<char> buffer(keep + 2);  // room for the delimiter slot getline needs

    std::string line;
    std::int64_t line_number = 0;  // 64-bit: counts every line of the file (see FileWindow)
    while (true) {
        in.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto extracted = in.gcount();
        if (extracted == 0 && in.eof()) break;
        ++line_number;

        bool overlong = false;
        if (in.fail() && !in.eof()) {
            // The line did not fit: getline set failbit without consuming the
            // delimiter. Clear it and drop the rest of the line WITHOUT storing
            // it -- that discard is the whole point.
            in.clear();
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            overlong = true;
        }

        const bool wanted = line_number >= offset_1based &&
                            (limit == 0 || line_number <= window_end);
        if (!wanted) {
            if (limit != 0 && line_number > window_end) window.truncated = true;
            continue;  // counted for total_lines, never materialized
        }

        line.assign(buffer.data());
        // A NUL the 8 KiB binary sniff did not see (it sits deeper in the
        // file) ends the C-string here: strlen stops at it, so the model was
        // handed a line that ended mid-content with nothing saying so -- a
        // corrupted view indistinguishable from a real short line.
        //
        // Detected by length, not by scanning for '\0' (getline always writes
        // its own terminator, which a scan cannot distinguish from content):
        // a NUL-free line's visible length is exactly `gcount()` when the
        // delimiter was not consumed (EOF-terminated or buffer-full line) or
        // one less when it was. Anything shorter means a NUL cut the string.
        const std::size_t visible = line.size();
        const std::size_t extracted_count = static_cast<std::size_t>(extracted);
        const bool has_nul = visible != extracted_count && visible + 1 != extracted_count;
        // A trailing "\r" survives getline on CRLF files and would otherwise be
        // rendered into the model's context as a stray control character.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (overlong || (max_line_length > 0 && static_cast<int>(line.size()) > max_line_length)) {
            if (max_line_length > 0) line.resize(static_cast<std::size_t>(max_line_length));
            line += kTruncationMarker;
        }
        if (has_nul) {
            line += kNulMarker;  // last, so the overlong resize cannot cut it
        }
        window.lines.push_back(std::move(line));
        line.clear();
    }

    window.total_lines = line_number;
    window.ok = true;

    if (!window.lines.empty()) {
        window.start_line = offset_1based;
        window.end_line = offset_1based + static_cast<int>(window.lines.size()) - 1;
    }
    // An offset past EOF is not an error: it is the natural end of paging, and
    // the caller reports it as an empty window rather than a failure.
    return window;
}

}  // namespace minimal::tools
