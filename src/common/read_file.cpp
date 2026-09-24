#include "minimal/common/read_file.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace minimal::common {

ReadFileResult ReadFile(const std::filesystem::path& path, std::size_t max_bytes) {
    using Status = ReadFileResult::Status;
    ReadFileResult result;

    // Some platforms (macOS) open a directory as a stream that reads as empty;
    // it must not pass for an empty file.
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        result.status = Status::kCannotOpen;
        result.error = "could not open " + path.string() + ": is a directory";
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const int error = errno;
        result.status = error == ENOENT ? Status::kNotFound : Status::kCannotOpen;
        result.error = "could not open " + path.string() + ": " +
                       std::error_code(error, std::generic_category()).message();
        return result;
    }

    // Bounded chunks, not `buffer << in.rdbuf()`: the cap must stop the read,
    // not be checked after the whole file is already in memory. Reading one
    // byte PAST the cap is what distinguishes "too big" from "exactly at the
    // cap". Deliberately not std::filesystem::file_size: that re-resolves the
    // path (a TOCTOU against the open stream) and answers 0 for things that
    // are not regular files.
    char chunk[64 * 1024];
    while (result.contents.size() <= max_bytes) {
        const auto want = std::min(sizeof(chunk), max_bytes + 1 - result.contents.size());
        in.read(chunk, static_cast<std::streamsize>(want));
        const auto got = in.gcount();
        if (got <= 0) break;
        result.contents.append(chunk, static_cast<std::size_t>(got));
    }

    // eofbit/failbit are how a short final read announces itself; only badbit
    // is a genuine I/O error.
    if (in.bad()) {
        result.status = Status::kIoError;
        result.error = "read failed (" + path.string() + "): I/O error";
        result.contents.clear();
    } else if (result.contents.size() > max_bytes) {
        result.status = Status::kTooLarge;
        result.error = "file exceeds the " + std::to_string(max_bytes) +
                       " byte read cap: " + path.string();
        result.contents.clear();
    }
    return result;
}

std::string ReadFileOrEmpty(const std::filesystem::path& path, std::size_t max_bytes) {
    ReadFileResult result = ReadFile(path, max_bytes);
    switch (result.status) {
        case ReadFileResult::Status::kOk: return std::move(result.contents);
        case ReadFileResult::Status::kNotFound:
        case ReadFileResult::Status::kCannotOpen: return {};
        case ReadFileResult::Status::kIoError:
        case ReadFileResult::Status::kTooLarge: throw std::runtime_error(result.error);
    }
    return {};
}

}  // namespace minimal::common
