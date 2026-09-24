#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace minimal::common {

// The outcome of reading a whole file under a size cap. `error` is a
// human-readable reason naming the path, set whenever status != kOk.
struct ReadFileResult {
    enum class Status {
        kOk,
        kNotFound,    // open failed with ENOENT
        kCannotOpen,  // open failed otherwise (permissions, a directory, ...)
        kIoError,     // I/O error mid-read; nothing is returned
        kTooLarge,    // more than max_bytes; nothing is returned
    };
    Status status = Status::kOk;
    std::string contents;
    std::string error;

    bool ok() const { return status == Status::kOk; }
};

// Reads the whole file, never more than max_bytes + 1 bytes into memory. The
// default cap is generous -- callers read prompts, presets and small JSON
// documents, not data files. An empty file is kOk with empty contents.
ReadFileResult ReadFile(const std::filesystem::path& path, std::size_t max_bytes = 64u << 20);

// ReadFile for callers where "missing" and "empty" mean the same thing:
// returns "" when the file cannot be opened, and throws std::runtime_error
// when it is too large or an I/O error occurs mid-read (a silently truncated
// string is worse than a loud failure).
std::string ReadFileOrEmpty(const std::filesystem::path& path,
                            std::size_t max_bytes = 64u << 20);

}  // namespace minimal::common
