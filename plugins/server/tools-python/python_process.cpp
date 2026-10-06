#include "python_process.hpp"

#include <unistd.h>

#include <utility>

#include "minimal/sidecar/drain.hpp"
#include "minimal/common/units.hpp"

namespace minimal::tools_python {

LaunchedInterpreter LaunchInterpreter(const std::vector<std::string>& argv,
                                      const std::filesystem::path& cwd,
                                      const std::map<std::string, std::string>& env,
                                      std::int64_t cpu_seconds, std::int64_t file_size_bytes) {
    // The launch shape, over the sidecar core's generic launcher. Five pipes:
    // the protocol stdin (parent keeps the write end), the diagnostics pipe
    // (the child's REAL stdout+stderr -- executed code writing to fd 1/2 lands
    // there instead of corrupting the protocol stream), the protocol stdout on
    // fd 3 -- a passthrough fd, so its CLOEXEC must be cleared explicitly or
    // exec closes it -- the live stdout stream on fd 4 (same passthrough
    // shape), and the user stdin read end on fd 5 (likewise). Protocol stdin
    // covers fd 0, so the launcher's /dev/null fallback does not apply.
    minimal::sidecar::LaunchSpec spec;
    spec.argv = argv;
    spec.cwd = cwd;
    spec.env = env;
    spec.pipes = {
        minimal::sidecar::PipePlan{minimal::sidecar::PipePlan::ChildEnd::kRead,
                                   {{STDIN_FILENO, false}}},
        minimal::sidecar::PipePlan{minimal::sidecar::PipePlan::ChildEnd::kWrite,
                                   {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
        minimal::sidecar::PipePlan{minimal::sidecar::PipePlan::ChildEnd::kWrite,
                                   {{kProtoStdoutFd, true}}},
        minimal::sidecar::PipePlan{minimal::sidecar::PipePlan::ChildEnd::kWrite,
                                   {{kStreamFd, true}}},
        minimal::sidecar::PipePlan{minimal::sidecar::PipePlan::ChildEnd::kRead,
                                   {{kUserStdinFd, true}}},
    };
    spec.cpu_seconds = cpu_seconds;
    spec.file_size_bytes = file_size_bytes;

    auto launched = minimal::sidecar::Launch(spec);
    LaunchedInterpreter proc;
    proc.ok = launched.ok;
    proc.spawn_error = std::move(launched.spawn_error);
    proc.pid = launched.pid;
    proc.warnings = std::move(launched.warnings);
    if (launched.ok && launched.parent_fds.size() == 5) {
        proc.in_fd = launched.parent_fds[0].release();
        proc.diag_fd = launched.parent_fds[1].release();
        proc.out_fd = launched.parent_fds[2].release();
        proc.stream_fd = launched.parent_fds[3].release();
        proc.user_in_fd = launched.parent_fds[4].release();
    }
    return proc;
}

CapturedRun RunCaptured(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                        const std::map<std::string, std::string>& env,
                        std::int64_t timeout_seconds, std::size_t max_output_bytes) {
    CapturedRun run;

    minimal::sidecar::LaunchSpec spec;
    spec.argv = argv;
    spec.cwd = cwd;
    spec.env = env;
    // Merged stdout+stderr; stdin is /dev/null (no pipe covers fd 0).
    spec.pipes = {minimal::sidecar::PipePlan{
        minimal::sidecar::PipePlan::ChildEnd::kWrite,
        {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}}};
    // RLIMIT_CPU backstop: the wall-clock deadline normally fires first; the
    // grace keeps the two from racing on a slow-but-healthy install.
    spec.cpu_seconds = timeout_seconds + 30;
    spec.file_size_bytes = common::kGiB;

    auto launched = minimal::sidecar::Launch(spec);
    if (!launched.ok) {
        run.error = launched.spawn_error;
        return run;
    }

    std::vector<minimal::sidecar::PipeSink> sinks(1);
    sinks[0].mode = minimal::sidecar::PipeSink::Mode::kCapped;
    sinks[0].cap = static_cast<std::int64_t>(max_output_bytes);

    minimal::sidecar::DrainOptions options;
    options.timeout_ms = timeout_seconds * 1000;

    const auto outcome = minimal::sidecar::DrainAndReap(
        std::move(launched), std::move(sinks), options, {});

    run.output = outcome.sinks[0].text;
    if (outcome.timed_out) {
        run.exit_status = -1;
        run.error = "timed out after " + std::to_string(timeout_seconds) +
                    "s; process group killed";
        return run;
    }
    if (outcome.reap_error) {
        // The pid was reaped out from under us; the status is meaningless.
        run.exit_status = -1;
        return run;
    }
    run.ok = true;
    run.exit_status =
        WIFEXITED(outcome.wait_status) ? WEXITSTATUS(outcome.wait_status) : -1;
    return run;
}

}  // namespace minimal::tools_python
