#include "commands_md.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

#include "minimal/commands/command_context.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/frontmatter.hpp"
#include "minimal/sidecar/drain.hpp"
#include "minimal/sidecar/process.hpp"

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

// The allowlisted slice of the server's environment -- the child's FULL
// environment (sidecar::LaunchSpec::env is not a delta).
std::map<std::string, std::string> BuildAllowlistedEnv() {
    std::map<std::string, std::string> env;
    if (environ == nullptr) return env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (equals == nullptr || equals == *entry) continue;  // malformed / empty key
        std::string key(*entry, equals - *entry);
        const bool allowed =
            std::any_of(std::begin(kEnvAllowlist), std::end(kEnvAllowlist),
                        [&](const char* name) { return key == name; }) ||
            std::any_of(std::begin(kEnvAllowlistPrefixes), std::end(kEnvAllowlistPrefixes),
                        [&](const char* prefix) { return key.rfind(prefix, 0) == 0; });
        if (allowed) env.emplace(std::move(key), equals + 1);
    }
    return env;
}

// --- process runner -----------------------------------------------------------
//
// Over the shared sidecar launcher (src/server/sidecar), not a private
// fork/exec: it owns the async-signal-safety rules, setsid + group kill, the
// rlimit backstops, the process-exit pgid sweep, stdin=/dev/null, and the
// bounded reap. What stays here is this plugin's own policy: stdout is the
// value, stderr the error narrative, and the output cap KILLS the generator
// and keeps the capped bytes (the confirm flow submits exactly those -- a
// second, uncapped run would repeat the generator's side effects).

struct RunOutcome {
    bool ok = false;
    std::string output;       // stdout (exactly cap-bounded when exceeded_cap)
    std::string error;        // reason when !ok, stderr included
    bool exceeded_cap = false;
    bool cancelled = false;   // `cancelled` fired: the user cancelled, or the session ended
};

// stderr is only ever an error narrative: head+tail is plenty, and a chatty
// generator's final lines (where the actual failure usually is) survive.
constexpr std::int64_t kStderrCapBytes = 4000;

// DrainOptions has no "no deadline"; `timeout: 0` (documented as infinite)
// maps to a deadline no generator run will meet.
constexpr std::int64_t kNoTimeoutMs = 30LL * 24 * 3600 * 1000;

RunOutcome RunProcess(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                      int timeout_seconds, std::size_t cap_bytes,
                      const std::function<bool()>& cancelled) {
    RunOutcome outcome;

    sidecar::LaunchSpec spec;
    spec.argv = argv;
    spec.cwd = cwd;
    spec.env = BuildAllowlistedEnv();
    spec.pipes = {
        sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite, {{STDOUT_FILENO, false}}},
        sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite, {{STDERR_FILENO, false}}},
    };
    // RLIMIT_CPU backstop; the wall-clock deadline normally fires first, and
    // the grace keeps the two from racing on a generator that uses its budget.
    if (timeout_seconds > 0) spec.cpu_seconds = timeout_seconds + 30;

    auto launched = sidecar::Launch(spec);
    if (!launched.ok) {
        outcome.error = "could not execute: " + launched.spawn_error;
        return outcome;
    }
    if (!launched.warnings.empty()) common::DefaultLogger().warn("{}", launched.warnings);

    // stdout accumulates HERE, through the kCapped sink's live tap, so the cap
    // can stop the run: the tap latches the overrun and StopCheck -- polled at
    // the top of every drain slice -- ends the drain, which kills the group.
    // The sink's own buffer is unused, hence the token cap of 1.
    std::string out_buf;
    bool exceeded_cap = false;
    std::vector<sidecar::PipeSink> sinks(2);
    sinks[0].mode = sidecar::PipeSink::Mode::kCapped;
    sinks[0].cap = 1;
    sinks[0].emit = [&](std::string_view chunk) {
        if (exceeded_cap) return;
        const std::size_t room = cap_bytes - out_buf.size();
        out_buf.append(chunk.substr(0, room));
        // Zero is a real cap (`output_cap: {max: 0}` = no output tolerated),
        // never "unset": cap_bytes is always concrete.
        if (chunk.size() > room) exceeded_cap = true;
    };
    sinks[1].mode = sidecar::PipeSink::Mode::kCapped;
    sinks[1].cap = kStderrCapBytes;

    sidecar::DrainOptions options;
    options.timeout_ms =
        timeout_seconds > 0 ? static_cast<std::int64_t>(timeout_seconds) * 1000 : kNoTimeoutMs;

    // Polled every drain slice (50 ms): a cancel kills the generator's group
    // within one slice instead of leaving the session worker parked until the
    // source's timeout (600 s by default).
    const auto drained = sidecar::DrainAndReap(
        std::move(launched), std::move(sinks), options,
        [&exceeded_cap, &cancelled] { return exceeded_cap || (cancelled && cancelled()); });
    const std::string& err_text = drained.sinks[1].text;
    const auto with_stderr = [&err_text](std::string message) {
        if (!err_text.empty()) message += "; stderr: " + err_text;
        return message;
    };

    if (exceeded_cap) {
        outcome.exceeded_cap = true;
        outcome.output = std::move(out_buf);
        return outcome;
    }
    if (drained.cancelled) {
        outcome.cancelled = true;
        outcome.error = "cancelled";
        return outcome;
    }
    if (drained.timed_out) {
        outcome.error = with_stderr("timed out after " + std::to_string(timeout_seconds) + "s");
        return outcome;
    }
    // An I/O failure mid-drain or a lost reap: the buffered stdout may be
    // partial and the exit status is unknown -- never ship it as a success.
    if (drained.io_error) {
        outcome.error = "reading generator output failed: " + drained.io_error_message;
        return outcome;
    }
    if (drained.reap_error) {
        outcome.error = "generator exit status could not be determined";
        return outcome;
    }
    const int status = drained.wait_status;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // 127 included: from the launcher it means chdir/exec failed, from
        // `sh -c` an inner command was not found -- either way stderr says
        // which, so it is reported rather than guessed at.
        outcome.error = with_stderr(
            "exit status " + (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status))
                                                : "signal " + std::to_string(WTERMSIG(status))));
        return outcome;
    }

    outcome.ok = true;
    outcome.output = std::move(out_buf);
    return outcome;
}

// --- template rendering --------------------------------------------------------

// Single pass, values inserted verbatim, never re-scanned. KNOWN-NAMES ONLY:
// a {token} that matches a fillable name (positional, declared argument,
// source name, output field) substitutes; ANYTHING else -- JSON examples, awk
// programs, shell parameter expansion in the prompt's own prose -- passes
// through verbatim. The strict predecessor threw on every unknown token,
// which made brace-bearing prose unusable: a body that merely SHOWED the
// model a JSON example could never run at all. Typo detection survives for
// what typos actually look like: an identifier-shaped unknown token warns
// once per render ({agr} names no argument -- almost certainly a mistyped
// {arg}), while prose shapes ({"type": ...}, {print $1}) stay quiet.
//
// An unknown token emits only its '{' and scanning resumes at the NEXT
// character, not past the '}': in `{"q": "{args}"}` the first '{' opens no
// known name, and skipping to the first '}' would swallow the real {args}
// placeholder inside it. Still single-pass over the template; substituted
// values are appended and never re-scanned.
bool LooksLikePlaceholder(const std::string& token) {
    if (token.empty()) return false;
    if (!std::isalnum(static_cast<unsigned char>(token.front())) &&
        token.front() != '_') {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' ||
               c == ':' || c == '-';
    });
}

std::string RenderTemplate(const std::string& body,
                           const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(body.size());
    std::set<std::string> warned;
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
            // Literal, not a mistake -- unless the token is shaped like a
            // placeholder name, in which case warn (once per render) and pass
            // it through anyway. A deliberate unknown must not kill the
            // command; a silent one must not vanish from the log.
            if (LooksLikePlaceholder(token) && warned.insert(token).second) {
                common::DefaultLogger().warn(
                    "command body: '{{{}}}' matches no argument or source name; left as "
                    "literal text",
                    token);
            }
            out += body[i++];
            continue;
        }
        out += it->second;
        i = close + 1;
    }
    return out;
}

// Where a placeholder sits in a `sh:` script, as far as quoting goes.
enum class ShellQuoting { kNone, kSingle, kDouble };

// Renders `value` as literal text for the quoting context it lands in, so the
// author's own quotes keep working and the value can never end them:
//   - bare: POSIX single-quote wrapping, every `'` as `'\''`;
//   - inside '...': the same, minus the outer quotes the author already wrote;
//   - inside "...": backslash-escape the four characters special there
//     (\ " $ `).
// Context-blind wrapping turned opencode's idiomatic `echo "$1"` into
// `echo "'foo'"`, which prints the quotes.
std::string ShellEscape(const std::string& value, ShellQuoting context) {
    std::string out;
    if (context == ShellQuoting::kNone) out += '\'';
    for (const char c : value) {
        if (context == ShellQuoting::kDouble) {
            if (c == '\\' || c == '"' || c == '$' || c == '`') out += '\\';
            out += c;
        } else if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    if (context == ShellQuoting::kNone) out += '\'';
    return out;
}

// Substitutes {name} into one string (an argv entry or a shell script).
// LENIENT by design: only KNOWN names substitute; anything else in braces
// (JSON payloads, awk programs, shell parameter expansion) passes through
// verbatim, with the same resume-at-the-next-character rule as
// RenderTemplate. Returns false when the string names an unfilled optional --
// the caller omits the whole entry, which is what "optional" means on a
// command line.
//
// `shell`: true for a `sh:` script, false for a `run:` argv entry. A `run:`
// entry reaches execve() directly -- no shell ever parses it, so the
// substituted text cannot be reinterpreted regardless of its content. A `sh:`
// script IS parsed by /bin/sh, so an unescaped substitution lets the VALUE
// supply shell syntax (quotes, `;`, `|`, command substitution) that escapes
// wherever the author placed the placeholder -- true of a source's own STDOUT
// as much as a user-typed argument, which is why the caller additionally
// restricts which values a `sh:` script may see at all (only user-supplied
// arguments, never another source's output -- see the `use_shell` branch in
// Execute()). Values are escaped for the quoting context the scan is in (see
// ShellEscape); the scan tracks '...', "..." and backslash escapes, which is
// all of POSIX quoting a placeholder can sit inside.
bool SubstituteEntry(const std::string& entry, const std::map<std::string, std::string>& values,
                     const std::set<std::string>& omittable, std::string& out,
                     bool shell = false) {
    ShellQuoting quoting = ShellQuoting::kNone;
    std::size_t i = 0;
    while (i < entry.size()) {
        const char c = entry[i];
        if (c != '{') {
            if (shell) {
                if (c == '\\' && quoting != ShellQuoting::kSingle && i + 1 < entry.size()) {
                    out += c;
                    out += entry[i + 1];
                    i += 2;
                    continue;
                }
                if (c == '\'' && quoting != ShellQuoting::kDouble) {
                    quoting = quoting == ShellQuoting::kSingle ? ShellQuoting::kNone
                                                               : ShellQuoting::kSingle;
                } else if (c == '"' && quoting != ShellQuoting::kSingle) {
                    quoting = quoting == ShellQuoting::kDouble ? ShellQuoting::kNone
                                                               : ShellQuoting::kDouble;
                }
            }
            out += c;
            ++i;
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
            out += entry[i++];
            continue;
        }
        if (omittable.count(token) != 0 && it->second.empty()) {
            return false;  // unfilled optional: the whole entry is omitted
        }
        out += shell ? ShellEscape(it->second, quoting) : it->second;
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
            // The per-turn model/agent overrides are disclosed on the same
            // prompt, and trigger it on their own: a sourceless untrusted
            // file can still route the turn to a preset with looser tool
            // permissions, and that must not happen silently either.
            const bool overrides = !spec_.model.empty() || !spec_.agent.empty();
            if (!spec_.trusted && (!spec_.sources.empty() || overrides)) {
                std::string detail =
                    "This command comes from a project-local file, not your own config.\n";
                if (!spec_.agent.empty()) detail += "Its turn runs as agent: " + spec_.agent + "\n";
                if (!spec_.model.empty()) detail += "Its turn runs on model: " + spec_.model + "\n";
                if (!spec_.sources.empty()) detail += "It will run:\n";
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

            // Generators run in the session's project root -- what `git ...`
            // in a command file means -- never the server's own cwd (which
            // is "/" under the client's auto-spawn). A test fake with no
            // root falls back to the process cwd.
            std::filesystem::path cwd = context.session().project_root();
            if (cwd.empty()) cwd = std::filesystem::current_path();
            // Made now, before any source runs: a cancel from here on stops
            // the running generator and every source after it.
            const std::function<bool()> cancelled = context.session().CancelProbe();

            for (const auto& source : spec_.sources) {
                if (cancelled()) {
                    return CommandResult::Error("/" + spec_.name +
                                                " was cancelled; nothing was submitted");
                }
                std::vector<std::string> argv;
                if (source.use_shell) {
                    std::string script;
                    // A shell string cannot omit pieces; an unfilled optional in
                    // a script is an error rather than a silent gap. arg_values
                    // (not values): see the comment above the source loop --
                    // only user-supplied arguments may reach a `sh:` script, and
                    // shell=true escapes even those for their quoting context,
                    // so they cannot supply shell syntax that escapes the
                    // position the author placed them in.
                    const bool substituted =
                        SubstituteEntry(source.script, arg_values, unfilled, script,
                                        /*shell=*/true);
                    if (!substituted) {
                        return CommandResult::Error(
                            "source '" + source.name +
                            "': an optional argument it references was not provided");
                    }
                    argv = {"/bin/sh", "-c", std::move(script)};
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
                    RunProcess(argv, cwd, source.timeout_seconds, cap_bytes, cancelled);
                if (run.cancelled) {
                    return CommandResult::Error("/" + spec_.name + " was cancelled while source '" +
                                                source.name + "' ran; nothing was submitted");
                }
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
                    // Submit the FIRST run's output, already bounded by the
                    // cap and kept by RunProcess on the cap path: a second,
                    // uncapped execution would fire the generator's side
                    // effects again (it was killed mid-run, so "run it once
                    // more" is not idempotent) with nothing bounding the
                    // result. The confirmation was asked about the capped
                    // artifact, so the capped artifact is what ships. The
                    // capped run reports !ok (it was killed), but with the
                    // operator's opt-in this cached output is the success.
                    run.ok = true;
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
            if (!spec_.sources.empty() && cancelled()) {
                return CommandResult::Error("/" + spec_.name +
                                            " was cancelled; nothing was submitted");
            }

            // The ONE sanctioned path: the composed text becomes the turn itself,
            // appended to history and answered like any user turn (§4.4). The
            // frontmatter's per-turn model/agent ride along -- this turn only;
            // the session's sticky choices are untouched.
            if (!context.session().SubmitTurnWithOverrides(
                    prompt, spec_.model.empty() ? std::nullopt
                                                : common::SplitModelRef(spec_.model),
                    spec_.agent)) {
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
