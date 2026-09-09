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
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

namespace {

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

}  // namespace

int main(int argc, char* argv[]) {
    const int result = Catch::Session().run(argc, argv);
    KillLingeringChildren();
    return result;
}
