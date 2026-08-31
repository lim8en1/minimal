#include "minimal/tools/atomic_write.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace minimal::tools {
namespace {

std::string Errno() { return std::strerror(errno); }

// RAII so the temp file is removed on every failure path -- otherwise a failed
// write litters the user's project directory with debris.
class ScopedTempFile {
public:
    ScopedTempFile(std::filesystem::path path, int fd) : path_(std::move(path)), fd_(fd) {}
    ~ScopedTempFile() {
        if (fd_ >= 0) ::close(fd_);
        if (!committed_ && !path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    int fd() const { return fd_; }
    const std::filesystem::path& path() const { return path_; }
    void set_committed() { committed_ = true; }
    void close_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    std::filesystem::path path_;
    int fd_ = -1;
    bool committed_ = false;
};

}  // namespace

WriteOutcome write_file(const std::filesystem::path& confined_abs_path, std::string_view content) {
    WriteOutcome outcome;

    const auto parent = confined_abs_path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        outcome.error = "parent directory does not exist: " + parent.string();
        return outcome;
    }
    if (std::filesystem::is_directory(confined_abs_path, ec)) {
        outcome.error = "is a directory: " + confined_abs_path.string();
        return outcome;
    }

    outcome.created = !std::filesystem::exists(confined_abs_path, ec);

    // Same directory as the target, so the rename below stays within one
    // filesystem and is therefore atomic.
    std::string temp_template = (parent / (confined_abs_path.filename().string() + ".XXXXXX")).string();
    std::vector<char> temp_path(temp_template.begin(), temp_template.end());
    temp_path.push_back('\0');

    const int fd = ::mkstemp(temp_path.data());
    if (fd < 0) {
        outcome.error = "cannot create temp file next to target: " + Errno();
        return outcome;
    }
    // Close-on-exec: the fd carries the file's new content, and a bash command
    // forked from a concurrent session's worker would otherwise inherit it.
    // Part of the fd-hygiene discipline; the bash pipes set theirs too.
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ScopedTempFile temp(std::filesystem::path(temp_path.data()), fd);

    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            outcome.error = "write failed: " + Errno();
            return outcome;
        }
        written += static_cast<std::size_t>(n);
    }

    // Mode for the inode the rename installs. The rename swaps the temp inode
    // in wholesale, so an existing file's permissions live or die with what the
    // temp file carries: the previous unconditional 0644 stripped executable
    // bits and widened 0600 private files on their next edit. An existing
    // target keeps its own mode; a new file gets the usual 0644.
    if (!outcome.created) {
        struct stat st {};
        if (::stat(confined_abs_path.c_str(), &st) != 0) {
            outcome.error = "cannot stat target to preserve its mode: " + Errno();
            return outcome;
        }
        if (::fchmod(fd, st.st_mode & 07777) != 0) {
            outcome.error = "cannot preserve file mode: " + Errno();
            return outcome;
        }
    } else {
        // mkstemp creates 0600; match what a normal file would get instead.
        std::filesystem::permissions(temp.path(),
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::others_read,
                                     std::filesystem::perm_options::replace, ec);
    }

    // Durability before visibility: fsync the contents (with the mode already
    // set, so both persist together), then rename. Renaming a file whose data
    // is still only in the page cache would make it visible before it is
    // persistent.
    if (::fsync(fd) != 0) {
        outcome.error = "fsync failed: " + Errno();
        return outcome;
    }
    temp.close_fd();

    std::filesystem::rename(temp.path(), confined_abs_path, ec);
    if (ec) {
        outcome.error = "rename failed: " + ec.message();
        return outcome;
    }
    temp.set_committed();

    // The rename is only durable once the DIRECTORY entry naming the new file
    // is. Best-effort: some filesystems do not support directory fsync, and the
    // write itself is already safe at that point.
    const int dir_fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        (void)::fsync(dir_fd);
        (void)::close(dir_fd);
    }

    outcome.ok = true;
    outcome.bytes_written = written;
    return outcome;
}

}  // namespace minimal::tools
