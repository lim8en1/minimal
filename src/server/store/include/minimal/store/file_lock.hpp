#pragma once

#include <cstdint>
#include <filesystem>

namespace minimal::store {

// Exclusive, process-lifetime lock over a SQLite database file, so the
// single-writer assumption the stores are built on is a property of the
// filesystem instead of a convention nothing enforces. Two live writers on one
// transcript would interleave appends with divergent in-memory working sets.
//
// Mechanics: an exclusive fcntl(F_SETLK) write lock on `<db_path>.lock`, held
// on an open fd for the lifetime of this object; the holder's pid is recorded
// in the file for diagnostics. fcntl locks die with the process, so a crash
// leaves no staleness to clean up. O_CLOEXEC keeps the fd out of every child,
// and O_NOFOLLOW keeps the truncating write off the far end of a planted
// symlink. After acquiring, the lock's identity is verified against the path
// (fstat of the fd vs lstat of the path) so a lock file unlinked and recreated
// mid-acquire cannot leave two processes locking two different inodes; a
// mismatch releases and retries, bounded.
//
// Same-process exclusion: fcntl locks never conflict within one process, so an
// in-process registry keyed by the lock file's (device, inode) refuses a second
// holder -- whatever path spelling reaches the file (`./`, a symlinked
// directory). The registry is consulted BEFORE a second fd is opened, and that
// ordering is load-bearing: closing ANY fd on a file drops every fcntl lock the
// process holds on it, so merely opening and closing the lock file to inspect
// it would silently release the first holder's lock.
//
// Refusal names the holder: construction throws when the lock is held, either
// by this process or by another one (EACCES/EAGAIN; the pid is read from the
// lock file, best-effort). Any OTHER fcntl errno is a filesystem that cannot
// lock (ENOLCK on NFS, ...) and is reported as itself, not as contention.
class ExclusiveFileLock {
public:
    // Throws std::runtime_error when the lock is held. db_path is the
    // DATABASE file; the lock file is db_path with ".lock" appended.
    explicit ExclusiveFileLock(const std::filesystem::path& db_path);
    ~ExclusiveFileLock();

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

private:
    std::filesystem::path lock_path_;
    int fd_ = -1;
    std::uint64_t dev_ = 0;  // the locked file's identity: the registry key
    std::uint64_t ino_ = 0;
};

}  // namespace minimal::store
