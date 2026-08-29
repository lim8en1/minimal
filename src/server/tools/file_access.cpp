#include "minimal/tools/file_access.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace minimal::tools {
namespace {

constexpr std::size_t kBinarySniffBytes = 8192;
constexpr const char* kTruncationMarker = "... [line truncated]";

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

    const int window_end = (limit == 0) ? 0 : offset_1based + limit - 1;

    std::string line;
    int line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;

        // A trailing "\r" survives getline on CRLF files and would otherwise be
        // rendered into the model's context as a stray control character.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line_number < offset_1based) continue;
        if (limit != 0 && line_number > window_end) {
            // Keep counting to report an accurate total, but stop retaining.
            window.truncated = true;
            continue;
        }

        if (max_line_length > 0 && static_cast<int>(line.size()) > max_line_length) {
            line.resize(static_cast<std::size_t>(max_line_length));
            line += kTruncationMarker;
        }
        window.lines.push_back(std::move(line));
        line = {};
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
