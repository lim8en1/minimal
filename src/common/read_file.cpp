#include "minimal/common/read_file.hpp"
#include "minimal/common/policy.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace minimal::common {
namespace {

// Closes on every return path.
struct Fd {
    int fd;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

}  // namespace

ReadFileResult ReadFile(const std::filesystem::path& path, std::size_t max_bytes) {
    using Status = ReadFileResult::Status;
    ReadFileResult result;
    const auto fail = [&](Status status, const std::string& error) {
        result.status = status;
        result.error = error;
        result.contents.clear();
        return result;
    };

    const Fd file{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0) {
        const int error = errno;
        return fail(error == ENOENT ? Status::kNotFound : Status::kCannotOpen,
                    "could not open " + path.string() + ": " + std::strerror(error));
    }

    // fstat on the open descriptor, not a stat of the path: what is checked
    // is what gets read. A directory opens O_RDONLY fine on POSIX and must not
    // pass for an empty file; a FIFO or device would read without end.
    struct stat st {};
    if (::fstat(file.fd, &st) != 0) {
        return fail(Status::kIoError, "could not stat " + path.string() + ": " + std::strerror(errno));
    }
    if (S_ISDIR(st.st_mode)) {
        return fail(Status::kCannotOpen, "could not open " + path.string() + ": is a directory");
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(Status::kCannotOpen, "could not open " + path.string() + ": not a regular file");
    }

    // Bounded reads: the cap stops the read rather than being checked after the
    // whole file is in memory. Reading one byte PAST the cap is what tells "too
    // big" from "exactly at the cap". st_size only sizes the first allocation;
    // the file may change size while being read.
    result.contents.reserve(std::min<std::size_t>(static_cast<std::size_t>(st.st_size), max_bytes) + 1);
    char chunk[kReadChunkBytes];
    while (result.contents.size() <= max_bytes) {
        const auto want = std::min(sizeof(chunk), max_bytes + 1 - result.contents.size());
        const ssize_t got = ::read(file.fd, chunk, want);
        if (got < 0) {
            if (errno == EINTR) continue;
            return fail(Status::kIoError, "read failed (" + path.string() + "): " + std::strerror(errno));
        }
        if (got == 0) break;
        result.contents.append(chunk, static_cast<std::size_t>(got));
    }

    if (result.contents.size() > max_bytes) {
        return fail(Status::kTooLarge, "file exceeds the " + std::to_string(max_bytes) +
                                           " byte read cap: " + path.string());
    }
    return result;
}

std::string ReadFileOrEmpty(const std::filesystem::path& path, std::size_t max_bytes) {
    ReadFileResult result = ReadFile(path, max_bytes);
    switch (result.status) {
        case ReadFileResult::Status::kOk: return std::move(result.contents);
        case ReadFileResult::Status::kNotFound: return {};
        // An unreadable file is not an absent one: callers that rewrite it
        // (plugins.json) or default it (hooks) would silently discard it.
        case ReadFileResult::Status::kCannotOpen:
        case ReadFileResult::Status::kIoError:
        case ReadFileResult::Status::kTooLarge: throw std::runtime_error(result.error);
    }
    return {};
}

}  // namespace minimal::common
