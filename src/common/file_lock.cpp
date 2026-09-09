#include "minimal/common/file_lock.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace minimal::common {
namespace {

// fcntl locks do not conflict with themselves across fds of one process, so
// the cross-process mechanism alone would let a second SessionDatabase on the
// same path open silently inside this process -- two live Sessions on one
// transcript, the exact corruption the lock exists to prevent. This registry
// is the same-process half of the guarantee; the fcntl lock is the
// cross-process half. Identity is the caller's path (the stores construct it
// deterministically from settings), guarded by this mutex.
std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<std::string>& HeldLocks() {
    static std::unordered_set<std::string> held;
    return held;
}

// Best-effort: the pid is written for the error message, not as a source of
// truth. A missing or unreadable file is fine -- the fcntl refusal carries
// the verdict, this only enriches it.
std::string ReadHolderPid(const std::filesystem::path& lock_path) {
    FILE* file = std::fopen(lock_path.c_str(), "r");
    if (file == nullptr) return "unknown";
    std::string pid;
    char buffer[64];
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (n == 0) return "unknown";
    buffer[n] = '\0';
    pid = buffer;
    while (!pid.empty() && (pid.back() == '\n' || pid.back() == '\r' || pid.back() == ' ')) {
        pid.pop_back();
    }
    return pid.empty() ? "unknown" : pid;
}

}  // namespace

ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path& db_path)
    : lock_path_(db_path.string() + ".lock") {
    {
        std::lock_guard lock(RegistryMutex());
        if (!HeldLocks().insert(lock_path_.string()).second) {
            throw std::runtime_error("database " + db_path.string() +
                                     " is already open in this process (pid " +
                                     std::to_string(::getpid()) + ")");
        }
    }

    // A fcntl lock belongs to the INODE behind the open fd, not to the path:
    // if the lock file is unlinked (and possibly recreated) between our open
    // and our F_SETLK -- a stale-file sweep is the realistic actor -- we lock
    // a ghost inode while a second process locks the fresh file at the same
    // path: two writers, one database. So after every acquire, fstat the OPEN
    // fd and stat the PATH, and only accept when they name the same file.
    // Bounded retries: each round re-opens whichever file now sits at the
    // path, so the retry re-locks the real thing; a path that keeps churning
    // under us is refused rather than raced indefinitely.
    constexpr int kMaxIdentityAttempts = 3;
    int fd = -1;
    for (int attempt = 0;; ++attempt) {
        // O_NOFOLLOW because of the ftruncate below: without it, anyone who can
        // create <db_path>.lock (the MINIMAL_DATA_DIR escape hatch can point at
        // a shared directory) can point it at a file they cannot write and have
        // this process truncate the target to zero for them.
        fd = ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            // Snapshotted first: taking the mutex and erasing from the set are
            // both allowed to clobber errno, and used to, so the message could
            // name an unrelated error.
            const int open_errno = errno;
            std::lock_guard lock(RegistryMutex());
            HeldLocks().erase(lock_path_.string());
            throw std::runtime_error("cannot open lock file " + lock_path_.string() + ": " +
                                     std::strerror(open_errno));
        }

        struct flock fl {};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (::fcntl(fd, F_SETLK, &fl) != 0) {
            // Snapshotted before ReadHolderPid, whose stdio calls clobber it.
            const int lock_errno = errno;
            // Only EACCES/EAGAIN mean "someone else holds it" -- POSIX allows
            // either. Everything else (ENOLCK on NFS, EINVAL/EOPNOTSUPP on some
            // FUSE mounts) used to be reported as contention too, which sent
            // the operator hunting a second instance that does not exist.
            const bool contended = lock_errno == EACCES || lock_errno == EAGAIN;
            const std::string holder = contended ? ReadHolderPid(lock_path_) : std::string();
            ::close(fd);
            std::lock_guard lock(RegistryMutex());
            HeldLocks().erase(lock_path_.string());
            if (!contended) {
                throw std::runtime_error("cannot lock " + lock_path_.string() + ": " +
                                         std::strerror(lock_errno) +
                                         " -- this filesystem may not support fcntl locks; "
                                         "put the database on local storage");
            }
            throw std::runtime_error("database " + db_path.string() +
                                     " is already in use by process " + holder +
                                     " (lock file " + lock_path_.string() +
                                     "); refusing to open -- stop the other instance first");
        }

        struct stat locked_st {};
        struct stat path_st {};
        // lstat, not stat, to pair with the O_NOFOLLOW above: if a symlink has
        // been swapped in at the path since our open, the honest answer is
        // "not the file we locked" -- following it would compare against the
        // target and call the swap a match.
        const bool same_file = ::fstat(fd, &locked_st) == 0 &&
                               ::lstat(lock_path_.c_str(), &path_st) == 0 &&
                               locked_st.st_dev == path_st.st_dev &&
                               locked_st.st_ino == path_st.st_ino;
        if (same_file) break;

        // Mismatch (a failed stat is a mismatch too: the file we locked was
        // unlinked under us). Release the ghost and try again.
        ::close(fd);
        fd = -1;
        if (attempt == kMaxIdentityAttempts - 1) {
            std::lock_guard lock(RegistryMutex());
            HeldLocks().erase(lock_path_.string());
            throw std::runtime_error("lock file " + lock_path_.string() +
                                     " kept changing under the lock attempt; refusing to open");
        }
    }

    // Record the holder for the loser's error message. Diagnostic only: a
    // failed write changes nothing about who holds the lock.
    (void)::ftruncate(fd, 0);
    (void)::dprintf(fd, "%ld\n", static_cast<long>(::getpid()));

    fd_ = fd;
}

ExclusiveFileLock::~ExclusiveFileLock() {
    if (fd_ >= 0) {
        ::close(fd_);  // releases the fcntl lock
        std::lock_guard lock(RegistryMutex());
        HeldLocks().erase(lock_path_.string());
    }
}

}  // namespace minimal::common
