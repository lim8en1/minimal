#pragma once

// Per-child memory ceiling (Linux). Without it a spawned child shares the
// server's cgroup -- on a desktop the terminal's or the tmux pane's scope --
// and a runaway build, script or interpreter that exhausts RAM gets the WHOLE
// scope killed by systemd-oomd: the child, the server, the client and the
// pane together. A scoped child instead runs in its own transient systemd
// user scope with MemoryMax, so the kernel OOM-kills it alone, inside it.

#include <string>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/sidecar/process.hpp"

namespace minimal::sidecar {

struct MemoryScope {
    std::string systemd_run;      // absolute path
    std::string xdg_runtime_dir;  // how systemd-run reaches the user manager's bus
};

// `command` prefixed with `systemd-run --user --scope ... -p MemoryMax=<p>%`.
// A scope (not a service) because systemd-run --scope moves ITSELF into the
// new unit and execs the command: the pid -- and the setsid'd group deadlines
// and cancellation kill -- stays the one Launch forked.
std::vector<std::string> ScopedArgv(const MemoryScope& scope, int memory_max_percent,
                                    std::vector<std::string> command);

// The usable scope, or null: not Linux, no systemd-run, or no reachable user
// manager (ssh without lingering, containers). Probed once per process on
// first use; the outcome is logged once, through the first caller's logger.
const MemoryScope* ProbeMemoryScope(common::Logger& logger);

// Rewrites `spec` to run under a MemoryMax=<percent>% scope when one is
// usable: argv[0] is resolved against this process's PATH first (systemd-run
// would resolve it against the CHILD's), and XDG_RUNTIME_DIR is added unless
// the spec already carries one. Returns whether the ceiling applies; a
// percent of 0 or no usable scope leaves `spec` untouched.
bool ApplyMemoryScope(LaunchSpec& spec, int memory_max_percent, common::Logger& logger);

}  // namespace minimal::sidecar
