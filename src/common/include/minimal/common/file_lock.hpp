#pragma once

#include <filesystem>

namespace minimal::common {

// Exclusive, process-lifetime lock over a SQLite database file, so the
// single-writer assumption the stores are built on is a property of the
// filesystem instead of a convention nothing enforces. Two live servers on one
// transcript would interleave appends with divergent in-memory working sets --
// worse than a crash -- and the in-process resume guard cannot see another
// process at all.
//
// Mechanics: an exclusive fcntl(F_SETLK) write lock on `<db_path>.lock`, held
// on an open fd for the lifetime of this object; the holder's pid is recorded
// in the file for diagnostics. fcntl locks die with the process, so a crash
// leaves no staleness to clean up. O_CLOEXEC keeps the fd out of every child
// (the bash tool, hook shells, MCP servers).
//
// Refusal names the holder: construction throws when the lock is held --
// either by another process (fcntl refuses; the pid is read from the lock
// file, best-effort) or by this process itself (an in-process registry, since
// fcntl locks do not conflict with themselves across fds).
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
};

}  // namespace minimal::common
