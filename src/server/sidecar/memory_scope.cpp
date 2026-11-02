#include "minimal/sidecar/memory_scope.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <optional>
#include <utility>

#include "minimal/common/env.hpp"
#include "minimal/sidecar/drain.hpp"
#include "minimal/sidecar/policy.hpp"

namespace minimal::sidecar {

std::vector<std::string> ScopedArgv(const MemoryScope& scope, int memory_max_percent,
                                    std::vector<std::string> command) {
    // --collect: a scope whose command was OOM-killed is "failed", and
    // without it lingers in the user manager until reset. --quiet: no
    // "Running as unit" line in the model-visible output.
    std::vector<std::string> argv = {scope.systemd_run,
                                     "--user",
                                     "--scope",
                                     "--quiet",
                                     "--collect",
                                     "-p",
                                     "MemoryMax=" + std::to_string(memory_max_percent) + "%",
                                     "--"};
    argv.insert(argv.end(), std::make_move_iterator(command.begin()),
                std::make_move_iterator(command.end()));
    return argv;
}

namespace {

#ifdef __linux__
std::optional<MemoryScope> Probe(common::Logger& logger) {
    const std::string systemd_run = ResolveOnPath("systemd-run");
    if (systemd_run.empty()) {
        logger.info("sidecar: systemd-run not found; children run without a memory ceiling");
        return std::nullopt;
    }
    const auto runtime_dir = common::LookupEnv("XDG_RUNTIME_DIR");
    if (!runtime_dir || runtime_dir->empty()) {
        logger.info("sidecar: XDG_RUNTIME_DIR unset (no user session); children run without "
                    "a memory ceiling");
        return std::nullopt;
    }
    MemoryScope scope{systemd_run, *runtime_dir};

    // The real invocation, end to end: a user manager that refuses transient
    // units (or a MemoryMax it cannot apply) must fail HERE, once, not every
    // spawn after it.
    LaunchSpec spec;
    spec.argv = ScopedArgv(scope, kMemoryScopeProbePercent, {"/bin/true"});
    spec.cwd = "/";
    spec.env = {{"XDG_RUNTIME_DIR", scope.xdg_runtime_dir}};
    spec.pipes = {PipePlan{PipePlan::ChildEnd::kWrite,
                           {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}}};
    auto launched = Launch(spec);
    if (!launched.ok) {
        logger.warn("sidecar: systemd-run probe failed to start ({}); children run without a "
                    "memory ceiling",
                    launched.spawn_error);
        return std::nullopt;
    }
    std::vector<PipeSink> sinks(1);
    sinks[0].mode = PipeSink::Mode::kCapped;
    sinks[0].cap = kMemoryScopeProbeOutputBytes;
    DrainOptions options;
    options.timeout_ms = kMemoryScopeProbeTimeout.count();
    const auto drained =
        DrainAndReap(std::move(launched), std::move(sinks), options, [] { return false; });
    const bool ok = !drained.timed_out && !drained.io_error && !drained.reap_error &&
                    WIFEXITED(drained.wait_status) && WEXITSTATUS(drained.wait_status) == 0;
    if (!ok) {
        logger.warn("sidecar: systemd-run --user --scope unusable{}{}; children run without a "
                    "memory ceiling",
                    drained.timed_out ? " (timed out)" : "",
                    drained.sinks[0].text.empty() ? "" : ": " + drained.sinks[0].text);
        return std::nullopt;
    }
    logger.info("sidecar: children run in transient systemd scopes with a MemoryMax ceiling");
    return scope;
}
#endif

}  // namespace

const MemoryScope* ProbeMemoryScope(common::Logger& logger) {
#ifdef __linux__
    static const std::optional<MemoryScope> scope = Probe(logger);
    return scope ? &*scope : nullptr;
#else
    (void)logger;
    return nullptr;
#endif
}

bool ApplyMemoryScope(LaunchSpec& spec, int memory_max_percent, common::Logger& logger) {
    if (memory_max_percent <= 0 || spec.argv.empty()) return false;
    const MemoryScope* scope = ProbeMemoryScope(logger);
    if (scope == nullptr) return false;
    // Unresolvable stays as-is: systemd-run then fails to exec it, and the
    // caller reports the child's exit like any other failed command.
    if (std::string resolved = ResolveOnPath(spec.argv[0]); !resolved.empty()) {
        spec.argv[0] = std::move(resolved);
    }
    spec.argv = ScopedArgv(*scope, memory_max_percent, std::move(spec.argv));
    spec.env.emplace("XDG_RUNTIME_DIR", scope->xdg_runtime_dir);
    return true;
}

}  // namespace minimal::sidecar
