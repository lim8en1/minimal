#include "minimal/common/fd_hygiene.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>

namespace minimal::common {

std::size_t MarkInheritedFdsCloseOnExec() {
    std::size_t marked = 0;

    // /proc/self/fd (Linux) and /dev/fd (macOS) both enumerate the calling
    // process's own descriptors. The listing directory's descriptor is
    // skipped; opendir marks it close-on-exec on the platforms this tree
    // targets, and even if one ever leaked it is self-healing on the next run.
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) dir = ::opendir("/dev/fd");
    if (dir == nullptr) return 0;
    const int dir_fd = ::dirfd(dir);

    while (const dirent* entry = ::readdir(dir)) {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0') continue;  // not a number
        if (fd <= 2 || fd == dir_fd) continue;
        const int flags = ::fcntl(static_cast<int>(fd), F_GETFD);
        if (flags < 0) continue;  // closed between the listing and now
        if ((flags & FD_CLOEXEC) != 0) continue;
        // Only counted on success: a failed F_SETFD leaves the descriptor
        // exactly as inheritable as before, and the count feeds a startup log
        // line callers read as "this many descriptors are now secured".
        if (::fcntl(static_cast<int>(fd), F_SETFD, flags | FD_CLOEXEC) != 0) continue;
        ++marked;
    }
    ::closedir(dir);
    return marked;
}

}  // namespace minimal::common
