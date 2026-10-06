#include "minimal/store/file_lock.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "minimal/store/policy.hpp"

namespace minimal::store {
namespace {

using FileId = std::pair<std::uint64_t, std::uint64_t>;  // (st_dev, st_ino)

// The same-process half of the guarantee (see the header). Held for the whole
// acquire, so two threads cannot both pass the check for one file.
std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::set<FileId>& HeldLocks() {
    static std::set<FileId> held;
    return held;
}

FileId IdOf(const struct stat& st) {
    return {static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
}

// Best-effort: the pid is written for the error message, not as a source of
// truth. Only called when ANOTHER process holds the lock (this process holds
// no lock on the file then, so the open/close here releases nothing).
std::string ReadHolderPid(const std::filesystem::path& lock_path) {
    FILE* file = std::fopen(lock_path.c_str(), "r");
    if (file == nullptr) return "unknown";
    // A decimal pid, sign, line ending and NUL.
    char buffer[std::numeric_limits<pid_t>::digits10 + 4];
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[n] = '\0';
    std::string pid = buffer;
    while (!pid.empty() && (pid.back() == '\n' || pid.back() == '\r' || pid.back() == ' ')) {
        pid.pop_back();
    }
    return pid.empty() ? "unknown" : pid;
}

}  // namespace

ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path& db_path)
    : lock_path_(db_path.string() + ".lock") {
    std::lock_guard registry(RegistryMutex());

    // In-process check first, without opening anything (see the header for
    // why opening is not allowed here). A lock file that does not exist yet
    // cannot be held by this process.
    if (struct stat existing {}; ::lstat(lock_path_.c_str(), &existing) == 0 &&
                                 HeldLocks().count(IdOf(existing)) != 0) {
        throw std::runtime_error("database " + db_path.string() +
                                 " is already open in this process (pid " +
                                 std::to_string(::getpid()) + ")");
    }

    // A fcntl lock belongs to the INODE behind the open fd, not to the path:
    // if the lock file is unlinked (and possibly recreated) between our open
    // and our F_SETLK, we would lock a ghost inode while a second process
    // locks the fresh file at the same path. So after every acquire, compare
    // the open fd with the path and only accept when they name the same file.
    // Each retry re-opens whatever now sits at the path; a path that keeps
    // churning is refused rather than raced indefinitely.
    for (int attempt = 0;; ++attempt) {
        const int fd = ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            throw std::runtime_error("cannot open lock file " + lock_path_.string() + ": " +
                                     std::strerror(errno));
        }

        struct flock fl {};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (::fcntl(fd, F_SETLK, &fl) != 0) {
            const int lock_errno = errno;  // before ReadHolderPid's stdio clobbers it
            // POSIX allows either errno for "someone else holds it".
            const bool contended = lock_errno == EACCES || lock_errno == EAGAIN;
            ::close(fd);
            if (!contended) {
                throw std::runtime_error("cannot lock " + lock_path_.string() + ": " +
                                         std::strerror(lock_errno) +
                                         " -- this filesystem may not support fcntl locks; "
                                         "put the database on local storage");
            }
            throw std::runtime_error("database " + db_path.string() +
                                     " is already in use by process " +
                                     ReadHolderPid(lock_path_) + " (lock file " +
                                     lock_path_.string() +
                                     "); refusing to open -- stop the other instance first");
        }

        // lstat, pairing with O_NOFOLLOW: a symlink swapped in at the path
        // since our open is "not the file we locked".
        struct stat locked_st {};
        struct stat path_st {};
        const bool same_file = ::fstat(fd, &locked_st) == 0 &&
                               ::lstat(lock_path_.c_str(), &path_st) == 0 &&
                               IdOf(locked_st) == IdOf(path_st);
        if (same_file) {
            fd_ = fd;
            std::tie(dev_, ino_) = IdOf(locked_st);
            break;
        }

        ::close(fd);  // release the ghost and try again
        if (attempt == kMaxLockIdentityAttempts - 1) {
            throw std::runtime_error("lock file " + lock_path_.string() +
                                     " kept changing under the lock attempt; refusing to open");
        }
    }

    HeldLocks().insert({dev_, ino_});

    // Record the holder for the loser's error message. Diagnostic only: a
    // failed truncate/write changes nothing about who holds the lock.
    // (An `if`, not a void cast: glibc's warn_unused_result ignores the cast.)
    if (::ftruncate(fd_, 0) != 0) {
    }
    (void)::dprintf(fd_, "%ld\n", static_cast<long>(::getpid()));
}

ExclusiveFileLock::~ExclusiveFileLock() {
    std::lock_guard registry(RegistryMutex());
    ::close(fd_);  // releases the fcntl lock
    HeldLocks().erase({dev_, ino_});
}

}  // namespace minimal::store
