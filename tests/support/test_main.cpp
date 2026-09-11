// Custom main for the test binaries: Catch2's main (Catch2WithMain) returns
// straight from run(), but a case that aborts mid-way (a failed REQUIRE skips
// the rest of the scope) never runs its own teardown. Spawned children -- the
// fake MCP servers, the real minimal-server in the E2E spawn test, background
// `sleep` tasks, python interpreters -- then outlive the binary while holding
// copies of its stdout/stderr. Under ctest those fds are the capture pipe, so
// ctest waits for EOF that never comes: the suite wedges after the failing
// test, and ctest's --timeout cannot help because the test process itself is
// already gone. Kill every descendant at process exit so the pipes close.

#include <catch2/catch_session.hpp>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#endif

namespace {

#if defined(__APPLE__)

void KillLingeringChildren() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) return;
    std::vector<kinfo_proc> procs(size / sizeof(kinfo_proc), kinfo_proc{});
    if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) != 0) return;
    const pid_t self = getpid();
    const pid_t self_pgrp = getpgrp();
    for (const auto& proc : procs) {
        if (proc.kp_eproc.e_ppid != self) continue;
        // Prefer the process group: a SIGKILLed child cannot reap its own
        // children, so grandchildren would survive and keep the pipe open.
        if (proc.kp_eproc.e_pgid != self_pgrp) {
            killpg(proc.kp_eproc.e_pgid, SIGKILL);
        } else {
            kill(proc.kp_proc.p_pid, SIGKILL);
        }
    }
}

#else  // Linux: /proc scan, same pgroup preference as above.

void KillLingeringChildren() {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    const pid_t self = getpid();
    const pid_t self_pgrp = getpgrp();
    while (const dirent* entry = readdir(proc)) {
        char* end = nullptr;
        const long pid = strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0) continue;

        // /proc/<pid>/stat: pid (comm) state ppid pgrp ... -- comm may contain
        // spaces, so parse the fields after the closing paren.
        //
        // stdio, not ifstream: this walk races the children it is about to
        // kill, and a process that exits between opendir and the read makes
        // libstdc++ THROW std::ios_base::failure out of filebuf::underflow
        // ("basic_filebuf::underflow error reading the file: No such
        // process"). Nothing here catches it, so it called std::terminate --
        // after a fully green run, main() aborting on the way out.
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        std::FILE* stat_file = std::fopen(path, "re");
        if (!stat_file) continue;
        char stat[512];
        const std::size_t filled = std::fread(stat, 1, sizeof(stat) - 1, stat_file);
        std::fclose(stat_file);
        stat[filled] = '\0';
        const char* paren = std::strrchr(stat, ')');
        if (paren == nullptr) continue;
        unsigned ppid = 0;
        unsigned pgrp = 0;
        if (std::sscanf(paren + 1, " %*c %u %u", &ppid, &pgrp) != 2) continue;

        if (static_cast<pid_t>(ppid) != self) continue;
        if (static_cast<pid_t>(pgrp) != self_pgrp) {
            killpg(static_cast<pid_t>(pgrp), SIGKILL);
        } else {
            kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
    closedir(proc);
}

#endif

}  // namespace

int main(int argc, char* argv[]) {
    const int result = Catch::Session().run(argc, argv);
    KillLingeringChildren();
    return result;
}
