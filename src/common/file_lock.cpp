#include "minimal/common/file_lock.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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

    const int fd = ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::lock_guard lock(RegistryMutex());
        HeldLocks().erase(lock_path_.string());
        throw std::runtime_error("cannot open lock file " + lock_path_.string() + ": " +
                                 std::strerror(errno));
    }

    struct flock fl {};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd, F_SETLK, &fl) != 0) {
        const std::string holder = ReadHolderPid(lock_path_);
        ::close(fd);
        std::lock_guard lock(RegistryMutex());
        HeldLocks().erase(lock_path_.string());
        throw std::runtime_error("database " + db_path.string() +
                                 " is already in use by process " + holder +
                                 " (lock file " + lock_path_.string() +
                                 "); refusing to open -- stop the other instance first");
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
