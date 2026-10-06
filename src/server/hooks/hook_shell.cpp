#include "minimal/hooks/hook_shell.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "minimal/common/utf8.hpp"
#include "minimal/sidecar/drain.hpp"
#include "minimal/sidecar/process.hpp"
#include "minimal/common/units.hpp"
#include "minimal/hooks/policy.hpp"

namespace minimal::hooks {
namespace {


std::string FirstLine(std::string_view captured) {
    std::string_view line;
    std::size_t start = 0;
    while (start < captured.size()) {
        const auto end = captured.find('\n', start);
        std::string_view candidate =
            captured.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                 : end - start);
        // Trim a trailing CR and surrounding whitespace.
        while (!candidate.empty() &&
               (candidate.back() == '\r' || candidate.back() == ' ' || candidate.back() == '\t')) {
            candidate.remove_suffix(1);
        }
        while (!candidate.empty() && (candidate.front() == ' ' || candidate.front() == '\t')) {
            candidate.remove_prefix(1);
        }
        if (!candidate.empty()) {
            line = candidate;
            break;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    // Cut on a character boundary and scrub: this is raw subprocess output
    // that becomes a user-visible reason string.
    return common::ScrubUtf8(common::TruncateUtf8Bytes(line, kMaxHookBlockReasonBytes));
}

}  // namespace

ShellActionResult RunShellAction(const std::string& command, std::string_view stdin_payload,
                                 std::chrono::milliseconds timeout,
                                 const std::function<bool()>& is_cancelled) {
    ShellActionResult result;

    // A zero timeout means "no budget", which a CANCELLABLE sync emission may
    // ask for -- its caller can still abandon the wait. The async path passes
    // a null canceller, though, and the hook worker is a single thread: zero
    // budget plus no cancellation means one wedged script blocks every later
    // hook, forever. Uncancellable runs therefore get a floor budget.
    const bool cancellable = is_cancelled != nullptr;
    const std::chrono::milliseconds effective =
        timeout.count() > 0 ? timeout : (cancellable ? std::chrono::milliseconds::zero()
                                                     : kUncancellableHookFloor);
    const bool budgeted = effective.count() > 0;

    // A hook is operator config, not model input: it runs with the server's
    // whole environment and working directory, as a plain `/bin/sh -c` would.
    // stdin is the payload pipe, never the server's; stdout and stderr merge
    // into one stream, in order.
    sidecar::LaunchSpec spec;
    spec.argv = {"/bin/sh", "-c", command};
    spec.env = sidecar::CurrentEnvironment();
    std::error_code cwd_error;
    spec.cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) spec.cwd = "/";
    spec.pipes = {
        sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite,
                          {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
        sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kRead, {{STDIN_FILENO, false}}},
    };
    // CPU and file-size backstops behind the wall-clock deadline, like the
    // bash tool's; none when there is no budget to back up.
    spec.cpu_seconds = sidecar::CpuBackstopSeconds(effective);
    spec.file_size_bytes = budgeted ? sidecar::kFileSizeBackstopBytes : 0;

    sidecar::Launched launched = sidecar::Launch(spec);
    result.warnings = launched.warnings;
    if (!launched.ok) {
        result.outcome = ShellActionOutcome::kFailOpen;
        result.fail_reason = launched.spawn_error;
        return result;
    }

    std::vector<sidecar::PipeSink> sinks(2);
    sinks[0].mode = sidecar::PipeSink::Mode::kCapped;
    sinks[0].cap = kMaxHookCapturedBytes;
    sinks[1].mode = sidecar::PipeSink::Mode::kSource;
    sinks[1].payload = std::string(stdin_payload);
    sidecar::DrainOptions options;
    options.timeout_ms = effective.count();
    const auto outcome = sidecar::DrainAndReap(
        std::move(launched), std::move(sinks), options,
        [&is_cancelled] { return is_cancelled && is_cancelled(); });

    if (outcome.cancelled || outcome.timed_out || outcome.io_error || outcome.reap_error) {
        result.outcome = ShellActionOutcome::kFailOpen;
        if (outcome.cancelled) {
            result.fail_reason = "cancelled";
        } else if (outcome.timed_out) {
            result.fail_reason = "timed out after " + std::to_string(effective.count()) + " ms";
        } else if (outcome.io_error) {
            result.fail_reason = "poll: " + outcome.io_error_message;
        } else {
            result.fail_reason = "exit status undetermined";
        }
        return result;
    }

    const int status = outcome.wait_status;
    if (!WIFEXITED(status)) {
        result.outcome = ShellActionOutcome::kFailOpen;
        result.fail_reason = WIFSIGNALED(status)
                                 ? "terminated by signal " + std::to_string(WTERMSIG(status))
                                 : "did not exit normally";
        return result;
    }

    const int code = WEXITSTATUS(status);
    if (code == 0) {
        result.outcome = ShellActionOutcome::kPass;
        return result;
    }
    if (code == 2) {
        result.outcome = ShellActionOutcome::kBlock;
        result.block_reason = FirstLine(outcome.sinks[0].text);
        if (result.block_reason.empty()) {
            result.block_reason = "blocked by hook (no reason given)";
        }
        return result;
    }

    result.outcome = ShellActionOutcome::kFailOpen;
    result.fail_reason = "exit status " + std::to_string(code);
    return result;
}

}  // namespace minimal::hooks
