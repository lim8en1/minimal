#include "commands_md.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <csignal>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <poll.h>

#include "minimal/commands/command_context.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/frontmatter.hpp"

// POSIX declares it in <unistd.h> only under feature-test macros that C++ mode
// does not guarantee; declare it ourselves. Same precedent as
// tools-bash/bash_session_state.cpp.
extern char** environ;

namespace minimal::commands_md {

namespace {

// --- environment allowlist ------------------------------------------------------
//
// A generator runs with a FILTERED environment, never the server's own: the
// server process's env carries every credential it holds (provider API keys
// resolved from api_key_env, operator exports), and a command file naming a
// generator that reads $ANTHROPIC_API_KEY must not be handed it silently.
// Mirrors tools-bash's allowlist (bash_session_state.cpp) -- not shared code
// (this plugin's own consumer-first rule: promote to a shared header only
// when a second real consumer appears), same intersection of "what a
// generator legitimately needs" ($PATH, $HOME, ...) and "nothing it has no
// business reading".
constexpr const char* kEnvAllowlist[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TERM_PROGRAM", "LANG", "TMPDIR",
};
constexpr const char* kEnvAllowlistPrefixes[] = {"LC_"};

// "KEY=VALUE" strings, verbatim from `environ`, filtered to the allowlist.
// Built in the PARENT: the child only ever dereferences this, never allocates.
std::vector<std::string> BuildAllowlistedEnv() {
    std::vector<std::string> env_strings;
    if (environ == nullptr) return env_strings;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (equals == nullptr || equals == *entry) continue;  // malformed / empty key
        const std::string key(*entry, equals - *entry);
        const bool allowed =
            std::any_of(std::begin(kEnvAllowlist), std::end(kEnvAllowlist),
                        [&](const char* name) { return key == name; }) ||
            std::any_of(std::begin(kEnvAllowlistPrefixes), std::end(kEnvAllowlistPrefixes),
                        [&](const char* prefix) { return key.rfind(prefix, 0) == 0; });
        if (allowed) env_strings.emplace_back(*entry);
    }
    return env_strings;
}

std::string AllowlistedPath(const std::vector<std::string>& env_strings) {
    for (const auto& entry : env_strings) {
        if (entry.rfind("PATH=", 0) == 0) return entry.substr(5);
    }
    return {};
}

// Every "<dir>/<name>" a PATH search of `argv0` would try, precomputed in the
// PARENT before fork -- execve() in the child must not allocate (async-signal
// safety; see the child_argv comment below). Mirrors execvp()'s own rule: a
// name containing '/' is used as-is, never PATH-searched.
std::vector<std::string> ExecCandidates(const std::string& argv0, const std::string& path) {
    if (argv0.empty() || argv0.find('/') != std::string::npos) return {argv0};
    std::vector<std::string> candidates;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto colon = path.find(':', start);
        const std::string dir =
            path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) candidates.push_back(dir + "/" + argv0);
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return candidates;
}

// --- process runner -----------------------------------------------------------
//
// One child, two pipes, one deadline. stdout is the value; stderr is the error
// narrative (read concurrently with stdout through poll, so a chatty stderr can
// never deadlock the run). On timeout the child is SIGKILLed and reaped. Output
// is capped at read time: once the cap is crossed the child is killed and the
// overrun reported -- the caller decides whether that is a question for the
// operator or a hard failure.

struct RunOutcome {
    bool ok = false;
    std::string output;       // stdout (empty when exceeded)
    std::string error;        // reason when !ok, stderr tail included
    bool exceeded_cap = false;
};

std::string Tail(std::string text, std::size_t max) {
    if (text.size() <= max) return text;
    return "..." + text.substr(text.size() - max);
}

// Both signals, always: killpg-equivalent (-pid) reaches the whole process
// group once the child's own setpgid(0,0) has run, but a kill landing in the
// fork-to-setpgid window targets a group that is still OUR group and misses
// the child entirely (ESRCH on the -pid form). kill(pid, ...) by plain pid
// always reaches the child regardless of that race. Same pairing as
// tools-bash's KillProcessGroup.
void KillGenerator(pid_t pid) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
}

RunOutcome RunProcess(const std::vector<std::string>& argv, const std::string& shell_script,
                      int timeout_seconds, std::size_t cap_bytes) {
    RunOutcome outcome;

    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        outcome.error = "pipe() failed";
        return outcome;
    }
    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        outcome.error = "pipe() failed";
        close(out_pipe[0]);
        close(out_pipe[1]);
        return outcome;
    }
    // O_CLOEXEC so these never leak into a THIRD process: without it, a
    // concurrent fork on another thread (another command, another bash-tool
    // invocation) can inherit a write end past its own exec and hold it open,
    // so this run's read() never sees EOF and blocks until its timeout even
    // though its own generator already exited.
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }

    // Everything the child touches, built HERE in the parent, before fork: a
    // forked child of a multithreaded process may call only async-signal-safe
    // functions until exec, and allocating (reserve/push_back/string
    // concatenation) is not one of them -- if another thread held the malloc
    // lock at fork time, the child would deadlock holding the pipe write end
    // open, which then wedges every OTHER concurrent RunProcess in this same
    // process via the read()-never-sees-EOF failure mode above. Mirrors
    // tools-bash/bash_process.cpp's "Built in the parent BEFORE fork" rule.
    const std::vector<std::string> env_strings = BuildAllowlistedEnv();
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (const auto& s : env_strings) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);

    std::vector<char*> child_argv;
    std::vector<std::string> exec_candidates;
    std::vector<char*> exec_candidate_ptrs;
    std::string shell_name = "sh";
    std::string shell_flag = "-c";
    if (!shell_script.empty()) {
        exec_candidates = {"/bin/sh"};
        child_argv = {shell_name.data(), shell_flag.data(),
                     const_cast<char*>(shell_script.c_str()), nullptr};
    } else {
        exec_candidates = ExecCandidates(argv.empty() ? std::string{} : argv.front(),
                                         AllowlistedPath(env_strings));
        child_argv.reserve(argv.size() + 1);
        for (const auto& arg : argv) child_argv.push_back(const_cast<char*>(arg.c_str()));
        child_argv.push_back(nullptr);
    }
    exec_candidate_ptrs.reserve(exec_candidates.size());
    for (const auto& c : exec_candidates) exec_candidate_ptrs.push_back(const_cast<char*>(c.c_str()));

    const pid_t pid = fork();
    if (pid < 0) {
        outcome.error = "fork() failed";
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return outcome;
    }
    if (pid == 0) {
        // Child. Only dereferences from here -- no allocation, see above. New
        // process group, so a timeout kill takes the whole tree (KillGenerator
        // covers the race before this call lands).
        setpgid(0, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        // Try every PATH candidate in order, same semantics as execvp -- but
        // over the ALLOWLISTED PATH baked into envp, and via execve so the
        // child's environment is the filtered one, never the server's own
        // (execl/execvp would inherit environ wholesale).
        for (char* candidate : exec_candidate_ptrs) {
            execve(candidate, child_argv.data(), envp.data());
        }
        _exit(127);  // exec failed; the parent reports through the exit status
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    // Deadline: poll timeout in ms; 0 s timeout means wait forever.
    const auto start = std::chrono::steady_clock::now();
    auto remaining_ms = [start, timeout_seconds]() -> int {
        if (timeout_seconds <= 0) return -1;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        const auto budget = std::chrono::milliseconds(timeout_seconds) * 1000;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(budget) - elapsed;
        return left.count() > 0 ? static_cast<int>(left.count()) : 0;
    };

    std::string out_buf;
    std::string err_buf;
    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;
    bool killed_for_cap = false;
    bool poll_failed = false;

    while (out_open || err_open) {
        pollfd fds[2];
        int count = 0;
        int out_index = -1;
        int err_index = -1;
        if (out_open) {
            out_index = count;
            fds[count++] = {out_pipe[0], POLLIN, 0};
        }
        if (err_open) {
            err_index = count;
            fds[count++] = {err_pipe[0], POLLIN, 0};
        }
        const int ready = poll(fds, static_cast<nfds_t>(count), remaining_ms());
        if (ready < 0) {
            if (errno == EINTR) continue;
            outcome.error = "poll() failed: " + std::string(std::strerror(errno));
            poll_failed = true;
            break;
        }
        if (ready == 0) {
            timed_out = true;
            break;
        }
        char chunk[4096];
        if (out_index >= 0 && (fds[out_index].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t got = read(out_pipe[0], chunk, sizeof(chunk));
            if (got <= 0) {
                out_open = false;
            } else {
                out_buf.append(chunk, static_cast<std::size_t>(got));
                if (cap_bytes > 0 && out_buf.size() > cap_bytes) {
                    killed_for_cap = true;
                    break;
                }
            }
        }
        if (err_index >= 0 && (fds[err_index].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t got = read(err_pipe[0], chunk, sizeof(chunk));
            if (got <= 0) {
                err_open = false;
            } else {
                err_buf.append(chunk, static_cast<std::size_t>(got));
                if (err_buf.size() > (1u << 20)) err_buf.resize(1u << 20);  // bounded narrative
            }
        }
    }

    // A poll() failure means the read loop can no longer be trusted to notice
    // the child exiting on its own -- kill it rather than let the bounded
    // reap below fall back to babysitting it for the full remaining budget.
    if (timed_out || killed_for_cap || poll_failed) {
        KillGenerator(pid);
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    // Bounded reap: the read loop exiting (both pipes at EOF, or a cap/timeout
    // break) says nothing about whether the PROCESS has exited -- a generator
    // that closes its own stdout/stderr and keeps running would otherwise wedge
    // this thread in an unbounded waitpid, silently ignoring the timeout the
    // command file declared. Poll WNOHANG under the SAME overall deadline the
    // read loop used (still unbounded when timeout_seconds<=0, by design: "0 =
    // infinite" is the documented contract, not a bug); a straggler past the
    // deadline is force-killed and reaped with one final blocking wait, which
    // returns promptly once SIGKILL (unblockable) lands.
    int status = 0;
    pid_t reaped = waitpid(pid, &status, WNOHANG);
    while (reaped == 0) {
        const int left_ms = remaining_ms();
        if (left_ms == 0) {
            KillGenerator(pid);
            waitpid(pid, &status, 0);
            break;
        }
        struct timespec poll_interval {
            0, 5 * 1000 * 1000  // 5ms
        };
        nanosleep(&poll_interval, nullptr);
        reaped = waitpid(pid, &status, WNOHANG);
    }

    if (timed_out) {
        outcome.error = "timed out after " + std::to_string(timeout_seconds) + "s";
        outcome.error += !err_buf.empty() ? "; stderr: " + Tail(err_buf, 2000) : "";
        return outcome;
    }
    if (killed_for_cap) {
        outcome.exceeded_cap = true;
        return outcome;
    }
    // poll_failed: outcome.error is already set above. Falling through to the
    // exit-status checks below would let a generator that happened to exit 0
    // ship its PARTIAL, still-buffered output as a real, successful turn --
    // exactly the one path where truncated/undefined output could reach the
    // model. Return now instead.
    if (poll_failed) return outcome;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        outcome.error = "could not execute: " + (argv.empty() ? shell_script : argv.front());
        return outcome;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        outcome.error = "exit status " +
                        (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status))
                                           : std::string("signal ") + std::to_string(WTERMSIG(status)));
        outcome.error += !err_buf.empty() ? "; stderr: " + Tail(err_buf, 2000) : "";
        return outcome;
    }

    outcome.ok = true;
    outcome.output = std::move(out_buf);
    (void)err_buf;
    return outcome;
}

// --- template rendering --------------------------------------------------------

// Single pass, values inserted verbatim, never re-scanned. Unknown placeholders
// are errors: a silent empty would submit a prompt missing exactly the piece
// the author meant to include.
std::string RenderTemplate(const std::string& body,
                           const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(body.size());
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '{') {
            out += body[i++];
            continue;
        }
        const auto close = body.find('}', i);
        if (close == std::string::npos) {
            out += body[i++];
            continue;
        }
        const std::string token = body.substr(i + 1, close - i - 1);
        const auto it = values.find(token);
        if (it == values.end()) {
            throw std::runtime_error("unknown placeholder {" + token + "} in command body");
        }
        out += it->second;
        i = close + 1;
    }
    return out;
}

// POSIX single-quote wrapping: every embedded `'` becomes `'\''` (close the
// quote, an escaped literal quote, reopen). The result is safe to splice into
// a /bin/sh -c string at any position a bare word or quoted string could
// legally appear -- unlike a value substituted unquoted, whose own content
// can supply the shell metacharacters that end the enclosing command.
std::string ShellQuote(const std::string& value) {
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// Substitutes {name} into one string (an argv entry or a shell script).
// LENIENT by design: only KNOWN names substitute; anything else in braces
// (JSON payloads, awk programs, shell parameter expansion) passes through
// verbatim. Returns false when the string names an unfilled optional -- the
// caller omits the whole entry, which is what "optional" means on a command
// line.
//
// `shell_quote`: true for a `sh:` script, false for a `run:` argv entry. A
// `run:` entry reaches execve() directly -- no shell ever parses it, so the
// substituted text cannot be reinterpreted regardless of its content. A `sh:`
// script IS parsed by /bin/sh, so an unquoted substitution lets the VALUE
// supply shell syntax (quotes, `;`, `|`, command substitution) that escapes
// wherever the author placed the placeholder -- true of a source's own STDOUT
// as much as a user-typed argument, which is why the caller additionally
// restricts which values a `sh:` script may see at all (only user-supplied
// arguments, never another source's output -- see the `use_shell` branch in
// Execute()).
bool SubstituteEntry(const std::string& entry, const std::map<std::string, std::string>& values,
                     const std::set<std::string>& omittable, std::string& out,
                     bool shell_quote = false) {
    std::size_t i = 0;
    while (i < entry.size()) {
        if (entry[i] != '{') {
            out += entry[i++];
            continue;
        }
        const auto close = entry.find('}', i);
        if (close == std::string::npos) {
            out += entry[i++];
            continue;
        }
        const std::string token = entry.substr(i + 1, close - i - 1);
        const auto it = values.find(token);
        if (it == values.end()) {
            // Unknown token: literal, not a mistake. The BODY render is where a
            // typo must fail loudly; a source argument is more likely shell/JSON
            // syntax than a mistyped parameter name. For a `sh:` script this ALSO
            // covers a {sourcename}/{output:...} reference: those keys are
            // deliberately absent from the restricted map Execute() passes in
            // here, so they fall through to this same literal-passthrough path
            // rather than substituting attacker-reachable generator output into
            // a shell string.
            out += entry.substr(i, close - i + 1);
            i = close + 1;
            continue;
        }
        if (omittable.count(token) != 0 && it->second.empty()) {
            return false;  // unfilled optional: the whole entry is omitted
        }
        out += shell_quote ? ShellQuote(it->second) : it->second;
        i = close + 1;
    }
    return true;
}

std::string JsonValueToText(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

}  // namespace

// --- the command ---------------------------------------------------------------

namespace {

class MdCommand : public commands::PromptShorthandCommand {
    using CommandResult = commands::CommandResult;
public:
    explicit MdCommand(MdCommandSpec spec) : spec_(std::move(spec)) {
        // Precomputed once: Usage() is a const override served to concurrent
        // callers (ListCommands RPC, /help pool, session workers). Writing
        // usage_ per call was an unsynchronized write to a mutable member --
        // undefined behavior -- and returned a view into a string that the
        // next call would resize out from under.
        usage_ = "/" + spec_.name;
        for (const auto& arg : spec_.args) usage_ += " <" + arg + ">";
        for (const auto& arg : spec_.optional) usage_ += " [" + arg + "]";
    }

    std::string_view Name() const override { return spec_.name; }
    std::string_view Summary() const override { return spec_.summary; }
    std::string_view Usage() const override { return usage_; }

    // Submits a turn by definition: never read-only, never a view.
    CommandResult Execute(commands::CommandContext& context,
                          const commands::CommandArgs& args) override {
        // --- bind ---------------------------------------------------------------
        // Unfilled optional names for THIS invocation (command objects are
        // shared across sessions; per-invocation state stays local).
        std::set<std::string> unfilled;
        // args[x] -> positional x+1; optional fills after the required ones; the
        // opencode desugared names ({args}, {1}..) come along for free.
        std::map<std::string, std::string> values;
        std::string joined;
        for (std::size_t i = 0; i < args.positional.size(); ++i) {
            if (i > 0) joined += " ";
            joined += args.positional[i];
            values[std::to_string(i + 1)] = args.positional[i];
        }
        values["args"] = joined;

        for (std::size_t i = 0; i < spec_.args.size(); ++i) {
            if (i >= args.positional.size()) {
                return CommandResult::Error("missing argument <" + spec_.args[i] + ">: usage " +
                                            std::string(Usage()));
            }
            values[spec_.args[i]] = args.positional[i];
        }
        for (std::size_t i = 0; i < spec_.optional.size(); ++i) {
            const std::size_t at = spec_.args.size() + i;
            // An unfilled optional binds as EMPTY for the template (rendering
            // nothing) and is recorded unfilled for the argv rule below (its
            // entry is omitted, not emptied).
            if (at < args.positional.size()) {
                values[spec_.optional[i]] = args.positional[at];
            } else {
                values[spec_.optional[i]] = "";
                unfilled.insert(spec_.optional[i]);
            }
        }

        // A `sh:` script may substitute ONLY these -- user-supplied arguments,
        // frozen before any source has run. `values` keeps growing as sources
        // execute (each adds its own name and, for JSON output, its fields),
        // and none of THAT may reach a shell string unquoted-or-not: it is
        // generator output, and generator output can be network data, git log
        // text, anything the file's author does not control (see B8/the
        // use_shell branch below). A `run:` argv entry is unaffected -- it
        // still sees the full, growing `values` map, since it reaches
        // execve() directly and no shell ever reinterprets it.
        const std::map<std::string, std::string> arg_values = values;

        // --- evaluate sources, in declaration order ------------------------------
        try {
            // An UNTRUSTED command (project-local: see MdCommandSpec::trusted)
            // must never fork a process silently. Disclose every declared
            // source's literal program/script (as authored -- {placeholder}
            // tokens included, not yet substituted; that is a more honest
            // disclosure than the resolved form, since it shows exactly what
            // code path the file's author chose) and block on the operator's
            // answer BEFORE any source runs. Asked on every invocation
            // (there is no session-scoped "always" grant here, unlike tool
            // permissions) -- repeated friction is the conservative default
            // for content the working directory supplied rather than the
            // operator configured.
            if (!spec_.trusted && !spec_.sources.empty()) {
                std::string detail =
                    "This command comes from a project-local file, not your own config, and "
                    "will run:\n";
                for (const auto& source : spec_.sources) {
                    detail += "  " + source.name + ": ";
                    if (source.use_shell) {
                        detail += "sh -c \"" + source.script + "\"\n";
                    } else {
                        for (std::size_t i = 0; i < source.argv.size(); ++i) {
                            if (i > 0) detail += " ";
                            detail += source.argv[i];
                        }
                        detail += "\n";
                    }
                }
                commands::ConfirmationRequest request;
                request.kind = "command_confirmation";
                request.title = "/" + spec_.name + ": run untrusted command file?";
                request.detail = std::move(detail);
                request.options = {"run", "cancel"};
                const auto answer = context.confirmations().RequestConfirmation(request);
                if (answer.chosen_option != "run") {
                    return CommandResult::Error("/" + spec_.name +
                                                " was not confirmed; nothing was run");
                }
            }

            for (const auto& source : spec_.sources) {
                std::vector<std::string> argv;
                std::string script;
                if (source.use_shell) {
                    // A shell string cannot omit pieces; an unfilled optional in
                    // a script is an error rather than a silent gap. arg_values
                    // (not values): see the comment above the source loop --
                    // only user-supplied arguments may reach a `sh:` script, and
                    // shell_quote=true so even those cannot supply shell syntax
                    // that escapes the position the author placed them in.
                    const bool substituted =
                        SubstituteEntry(source.script, arg_values, unfilled, script,
                                        /*shell_quote=*/true);
                    if (!substituted) {
                        return CommandResult::Error(
                            "source '" + source.name +
                            "': an optional argument it references was not provided");
                    }
                } else {
                    for (const auto& entry : source.argv) {
                        std::string piece;
                        // allow_omit: an entry naming an unfilled optional is
                        // OMITTED -- that is what "optional" means on a command
                        // line. An entry naming something unknown is an error.
                        if (SubstituteEntry(entry, values, unfilled, piece)) {
                            argv.push_back(std::move(piece));
                        }
                    }
                    if (argv.empty()) {
                        return CommandResult::Error("source '" + source.name +
                                                    "': every argv entry was omitted");
                    }
                }

                const std::size_t cap_bytes = source.cap_tokens * 4;
                RunOutcome run =
                    RunProcess(argv, script, source.timeout_seconds, cap_bytes);
                if (run.exceeded_cap) {
                    const std::size_t estimated = cap_bytes / 4;
                    if (source.on_exceed == CapAction::Fail) {
                        return CommandResult::Error(
                            "source '" + source.name + "' output exceeded the cap (~" +
                            std::to_string(estimated) + " tokens); nothing was submitted");
                    }
                    // Ask the operator. Over the cap there is no honest default:
                    // proceeding costs real tokens, aborting costs a keystroke.
                    commands::ConfirmationRequest request;
                    request.kind = "command_confirmation";
                    request.title = "output cap exceeded";
                    request.detail = "Source '" + source.name + "' produced more than ~" +
                                     std::to_string(estimated) +
                                     " tokens. Submit the composed prompt anyway?";
                    request.options = {"submit", "cancel"};
                    const auto answer = context.confirmations().RequestConfirmation(request);
                    if (answer.chosen_option != "submit") {
                        return CommandResult::Error("output cap confirmed nothing; nothing was "
                                                    "submitted");
                    }
                    // Re-run without the cap: the process was killed at the cap,
                    // so its output must be produced whole before submission.
                    run = RunProcess(argv, script, source.timeout_seconds, 0);
                    if (!run.ok) {
                        return CommandResult::Error("source '" + source.name + "': " + run.error);
                    }
                }
                if (!run.ok) {
                    return CommandResult::Error("source '" + source.name + "': " + run.error);
                }

                if (source.output == SourceOutput::kJson) {
                    nlohmann::json parsed;
                    try {
                        parsed = nlohmann::json::parse(run.output);
                    } catch (const std::exception& error) {
                        return CommandResult::Error("source '" + source.name +
                                                    "' did not emit valid JSON: " + error.what());
                    }
                    for (const auto& field : source.fields) {
                        if (!parsed.contains(field)) {
                            return CommandResult::Error("source '" + source.name +
                                                        "' output is missing field '" + field +
                                                        "'");
                        }
                        values["output:" + source.name + "." + field] =
                            JsonValueToText(parsed.at(field));
                    }
                }
                // Trailing newlines are echo/process artifacts, not prompt
                // content: strip them so a source inserted mid-sentence does
                // not break the line.
                while (!run.output.empty() &&
                       (run.output.back() == '\n' || run.output.back() == '\r')) {
                    run.output.pop_back();
                }
                values[source.name] = std::move(run.output);
            }

            // --- compose ---------------------------------------------------------
            std::string prompt;
            if (spec_.body.empty()) {
                // Whole-stdout mode: the sole text source's output IS the prompt.
                const Source* text_source = nullptr;
                for (const auto& source : spec_.sources) {
                    if (source.output == SourceOutput::kText) {
                        if (text_source != nullptr) {
                            return CommandResult::Error(
                                "no body and more than one text source; a body is required");
                        }
                        text_source = &source;
                    }
                }
                if (text_source == nullptr) {
                    return CommandResult::Error("no body and no text source to take the prompt "
                                                "from");
                }
                prompt = values.at(text_source->name);
            } else {
                prompt = RenderTemplate(spec_.body, values);
            }

            if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
                return CommandResult::Error("the composed prompt is empty; nothing was submitted");
            }

            // The ONE sanctioned path: the composed text becomes the turn itself,
            // appended to history and answered like any user turn (§4.4).
            if (!context.session().SubmitTurn(prompt)) {
                return CommandResult::Error("could not submit the turn (session busy?)");
            }
            return CommandResult::Ok("submitted '" + spec_.name + "' (" +
                                     std::to_string(prompt.size()) + " bytes)");
        } catch (const std::exception& error) {
            // Render/bind failures land here too: an error status line reaches
            // the operator, never the chat, never the model.
            return CommandResult::Error(std::string("command '") + spec_.name +
                                        "' failed: " + error.what());
        }
    }

private:
    MdCommandSpec spec_;
    std::string usage_;  // immutable after construction; see the constructor
};

}  // namespace

std::unique_ptr<commands::Command> MakeCommand(MdCommandSpec spec) {
    return std::make_unique<MdCommand>(std::move(spec));
}

}  // namespace minimal::commands_md
