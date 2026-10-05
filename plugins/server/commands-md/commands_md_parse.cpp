#include "commands_md.hpp"

// The .md frontmatter parser and the opencode desugarer. Everything here runs
// at LOAD time; invocation only binds, runs sources, and renders (see
// commands_md.cpp).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/frontmatter.hpp"

namespace minimal::commands_md {

namespace {

// --- opencode desugaring (parse-time; one engine at runtime) --------------------

// $ARGUMENTS -> {args}; $1..$9 -> {1}..{9}. Whole-token only (the next
// character must be a non-word boundary), so `$10x` never becomes "{1}0x"
// and `$ARGUMENTSX` never becomes "{args}X".
std::string DesugarPositionalTokens(const std::string& body) {
    const auto word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size();) {
        if (body[i] == '$' && body.compare(i, 10, "$ARGUMENTS") == 0) {
            const std::size_t after = i + 10;
            const bool boundary = after >= body.size() || !word_char(body[after]);
            if (boundary) {
                out += "{args}";
                i = after;
                continue;
            }
        }
        if (body[i] == '$' && i + 1 < body.size() && body[i + 1] >= '1' && body[i + 1] <= '9') {
            const std::size_t after = i + 2;
            const bool boundary = after >= body.size() || !word_char(body[after]);
            if (boundary) {
                out += '{';
                out += body[i + 1];
                out += '}';
                i = after;
                continue;
            }
        }
        out += body[i++];
    }
    return out;
}

// !`cmd` -> an anonymous `sh` source appended to `into`, and the token becomes
// {cmdN}. The command text is taken literally except for the positional
// desugaring below: it cannot reference other sources (that composition is
// what the frontmatter sources block is for), but `$1`/`$ARGUMENTS` inside it
// are the command's own arguments and desugar like anywhere else in the body.
//
// The generated name skips any name already taken (a declared source or
// argument called `cmd1`), so an anonymous source can never collide with an
// authored one.
std::string DesugarShellInjections(const std::string& body, std::vector<Source>& into,
                                   const std::set<std::string>& taken, bool& injected_any) {
    std::string out;
    std::size_t i = 0;
    int count = 0;
    while (i < body.size()) {
        if (body.compare(i, 2, "!`") == 0) {
            const auto close = body.find('`', i + 2);
            if (close == std::string::npos) {
                // Unterminated: leave the text alone; the render step will not
                // invent a meaning for it.
                out += body[i++];
                continue;
            }
            Source source;
            do {
                source.name = "cmd" + std::to_string(++count);
            } while (taken.count(source.name) != 0);
            source.use_shell = true;
            // Desugar positionals INSIDE the script too: `$1`/`$ARGUMENTS` in a
            // shell injection are the author addressing the command's own
            // arguments, exactly as in the body. The token was extracted from
            // the raw body here, so without this the script would carry `$1`
            // literally and /bin/sh would expand it to empty -- silently
            // dropping the argument the invocation supplied. Desugaring yields
            // {1}/{args}, which the use_shell substitute path fills from the
            // user-supplied arg values (and only those) at run time.
            source.script = DesugarPositionalTokens(body.substr(i + 2, close - i - 2));
            into.push_back(std::move(source));
            out += "{" + into.back().name + "}";
            injected_any = true;
            i = close + 1;
            continue;
        }
        out += body[i++];
    }
    return out;
}

// --- frontmatter decode ----------------------------------------------------------

std::string RequireScalar(const YAML::Node& node, const std::string& what) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("command file: " + what + " is required");
    }
    return node.as<std::string>();
}

std::vector<std::string> ScalarList(const YAML::Node& node, const std::string& what) {
    std::vector<std::string> out;
    if (!node) return out;
    if (!node.IsSequence()) {
        throw std::runtime_error("command file: " + what + " must be a list");
    }
    for (const auto& item : node) out.push_back(item.as<std::string>());
    return out;
}

Source DecodeSource(const std::string& name, const YAML::Node& node) {
    if (!node || !node.IsMap()) {
        throw std::runtime_error("command file: source '" + name + "' must be a map");
    }
    Source source;
    source.name = name;

    const YAML::Node run = node["run"];
    const YAML::Node sh = node["sh"];
    if (run && sh) {
        throw std::runtime_error("command file: source '" + name +
                                 "' must declare run or sh, not both");
    }
    if (run) {
        if (!run.IsSequence() || run.size() == 0) {
            throw std::runtime_error("command file: source '" + name +
                                     "' run must be a non-empty argv list");
        }
        for (const auto& item : run) source.argv.push_back(item.as<std::string>());
    } else if (sh) {
        source.use_shell = true;
        source.script = RequireScalar(sh, "source '" + name + "' sh");
    } else {
        throw std::runtime_error("command file: source '" + name +
                                 "' must declare run (argv) or sh (shell string)");
    }

    const YAML::Node output = node["output"];
    if (output) {
        const std::string type = RequireScalar(output["type"], "source '" + name + "' output.type");
        if (type == "json") {
            source.output = SourceOutput::kJson;
            source.fields = ScalarList(output["fields"], "source '" + name + "' output.fields");
            if (source.fields.empty()) {
                throw std::runtime_error("command file: source '" + name +
                                         "' declares json output with no fields");
            }
        } else if (type != "text") {
            throw std::runtime_error("command file: source '" + name +
                                     "' output.type must be text or json");
        }
    }

    const YAML::Node timeout = node["timeout"];
    if (timeout) {
        const int value = timeout.as<int>();
        if (value < 0) {
            throw std::runtime_error("command file: source '" + name + "' timeout must be >= 0");
        }
        // Clamped, not just floored: RunProcess's remaining_ms() narrows
        // timeout_seconds*1000 milliseconds into an `int` for poll()'s
        // deadline, and a value near INT_MAX overflowed that cast to a
        // negative int -- which poll() reads as "wait forever", the exact
        // opposite of a declared timeout. 86400s (24h) is generous for a
        // command generator and comfortably inside the safe range.
        constexpr int kMaxTimeoutSeconds = 86400;
        source.timeout_seconds = std::min(value, kMaxTimeoutSeconds);
    }

    // Truncated JSON never parses, so "submit the capped output anyway" has
    // nothing to submit: a JSON source fails on overrun by default, and an
    // explicit `on_exceed: confirm` is rejected rather than asked and then
    // failed.
    if (source.output == SourceOutput::kJson) source.on_exceed = CapAction::Fail;

    const YAML::Node cap = node["output_cap"];
    if (cap && cap.IsMap()) {
        if (cap["max"]) {
            const int tokens = cap["max"].as<int>();
            if (tokens < 0) {
                throw std::runtime_error("command file: source '" + name +
                                         "' output_cap.max must be >= 0");
            }
            source.cap_tokens = static_cast<std::size_t>(tokens);
        }
        if (cap["on_exceed"]) {
            const std::string action = cap["on_exceed"].as<std::string>();
            if (action == "confirm") {
                if (source.output == SourceOutput::kJson) {
                    throw std::runtime_error(
                        "command file: source '" + name +
                        "' output_cap.on_exceed: confirm cannot apply to json output "
                        "(truncated JSON does not parse); use fail");
                }
                source.on_exceed = CapAction::Confirm;
            } else if (action == "fail") {
                source.on_exceed = CapAction::Fail;
            } else {
                throw std::runtime_error("command file: source '" + name +
                                         "' output_cap.on_exceed must be confirm or fail");
            }
        }
    }
    return source;
}

// Every name that lands in the render map must mean one thing. Arguments and
// sources share that map (the bind step writes both), and the positional
// bindings `args` and `1`..`9` are always present -- so a duplicate, or a
// declared name equal to a built-in one, would silently overwrite one value
// with another in `{name}`. Runs AFTER desugaring so the anonymous `cmdN`
// sources are covered too.
void CheckNames(const MdCommandSpec& spec) {
    const auto reserved = [](const std::string& name) {
        return name == "args" || (name.size() == 1 && name[0] >= '1' && name[0] <= '9');
    };
    std::set<std::string> seen;
    const auto claim = [&](const std::string& name, const char* what) {
        if (reserved(name)) {
            throw std::runtime_error("command '" + spec.name + "': " + what + " '" + name +
                                     "' is reserved ({args} and {1}..{9} are the typed "
                                     "arguments) -- rename it");
        }
        if (name.find(':') != std::string::npos) {
            throw std::runtime_error("command '" + spec.name + "': " + what + " '" + name +
                                     "' may not contain ':' (the {output:...} namespace)");
        }
        if (!seen.insert(name).second) {
            throw std::runtime_error("command '" + spec.name + "': the name '" + name +
                                     "' is declared more than once across args, optional "
                                     "and sources -- rename one of them");
        }
    };
    for (const auto& name : spec.args) claim(name, "argument");
    for (const auto& name : spec.optional) claim(name, "optional argument");
    for (const auto& source : spec.sources) claim(source.name, "source");
}

MdCommandSpec DecodeSpec(const std::string& stem, const config::ParsedFrontmatter& parsed,
                         const std::filesystem::path& path, bool trusted) {
    MdCommandSpec spec;
    spec.name = stem;
    spec.source_file = path;
    spec.trusted = trusted;
    const YAML::Node& node = parsed.frontmatter;

    if (node && node.IsMap()) {
        spec.summary = node["description"] && node["description"].IsScalar()
                           ? node["description"].as<std::string>()
                           : "";
        spec.args = ScalarList(node["args"], "args");
        spec.optional = ScalarList(node["optional"], "optional");

        // opencode frontmatter minimal cannot honour without per-turn
        // overrides: `model` and `agent` now ride the per-turn path; `subtask`
        // (spawn-from-a-command) has no equivalent yet -- warn, do not
        // silently ignore.
        if (node["subtask"]) {
            common::DefaultLogger().warn(
                "command '{}' frontmatter field 'subtask' is not supported yet; ignored",
                spec.name);
        }
        if (node["model"] && node["model"].IsScalar()) {
            spec.model = node["model"].as<std::string>();
            // Shape-checked here so a typo warns at load, not at invocation;
            // the value still rides raw and is re-split (and resolved) at pop.
            if (!common::SplitModelRef(spec.model)) {
                throw std::runtime_error("command '" + spec.name +
                                         "': frontmatter 'model' must be \"<provider>/<model>\" (got \"" +
                                         spec.model + "\")");
            }
        }
        if (node["agent"] && node["agent"].IsScalar()) {
            spec.agent = node["agent"].as<std::string>();
        }

        const YAML::Node sources = node["sources"];
        if (sources && sources.IsMap()) {
            for (const auto& entry : sources) {
                spec.sources.push_back(DecodeSource(entry.first.as<std::string>(), entry.second));
            }
        }
    }

    // Desugar opencode constructs into the same flat source list. A file using
    // BOTH declared sources and !`cmd` constructs works (every construct is one
    // flat source) but is a style mix worth naming once.
    const std::size_t declared_sources = spec.sources.size();
    std::set<std::string> taken(spec.args.begin(), spec.args.end());
    taken.insert(spec.optional.begin(), spec.optional.end());
    for (const auto& source : spec.sources) taken.insert(source.name);
    bool injected_any = false;
    std::string desugared_body =
        DesugarShellInjections(parsed.body, spec.sources, taken, injected_any);
    spec.body = DesugarPositionalTokens(desugared_body);
    // A body of nothing but whitespace is no body: whole-stdout mode, not a
    // template that renders to an empty prompt.
    if (spec.body.find_first_not_of(" \t\r\n") == std::string::npos) spec.body.clear();
    CheckNames(spec);
    if (injected_any && declared_sources > 0) {
        common::DefaultLogger().warn(
            "command '{}' mixes frontmatter sources with !`cmd` constructs; prefer one style",
            spec.name);
    }
    return spec;
}

}  // namespace

MdCommandSpec ParseCommandFile(const std::filesystem::path& path, bool trusted) {
    // Unbounded read: the command dirs include PROJECT-LOCAL content (a
    // cloned repo), and a multi-gigabyte `.md` would be slurped whole at
    // every startup and listing. Anything past the cap is a broken command
    // file, skipped with a warning like any other.
    constexpr std::size_t kMaxCommandFileBytes = 1u << 20;  // 1 MiB
    common::ReadFileResult file = common::ReadFile(path, kMaxCommandFileBytes);
    if (!file.ok()) throw std::runtime_error(file.error);

    // The stem is what the user types after '/': anything outside this set
    // (a space, a quote) would register a command no one can invoke.
    const std::string stem = path.stem().string();
    const bool valid_name =
        !stem.empty() && std::isalnum(static_cast<unsigned char>(stem.front())) != 0 &&
        std::all_of(stem.begin(), stem.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
                   c == '.';
        });
    if (!valid_name) {
        throw std::runtime_error("command file: invalid name '" + stem +
                                 "' (letters, digits, '-', '_' and '.', starting with a "
                                 "letter or digit)");
    }
    return DecodeSpec(stem, config::parse_frontmatter_markdown(file.contents), path, trusted);
}

std::vector<std::unique_ptr<commands::Command>> LoadCommands(
    const std::vector<CommandDir>& dirs) {
    std::vector<std::unique_ptr<commands::Command>> out;
    // Nearest dir wins a stem collision: keep the first spec per stem in walk
    // order, and log what was shadowed. Callers list project directories
    // before the global one -- see CommandDir's doc -- so "nearest" and "first
    // in this loop" agree.
    std::map<std::string, std::filesystem::path> seen;
    std::vector<MdCommandSpec> specs;

    for (const auto& command_dir : dirs) {
        const std::filesystem::path& dir = command_dir.path;
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
            const std::string stem = entry.path().stem().string();
            // Shadowing is decided by NAME before parsing: a malformed project
            // file must not silently fall back to a global command the author
            // was overriding -- it is skipped with a warning like any other
            // broken file.
            if (!seen.emplace(stem, entry.path()).second) {
                common::DefaultLogger().warn("command '{}' from {} shadows {} (nearer directory "
                                             "wins)",
                                             stem, entry.path().string(),
                                             seen.at(stem).string());
                continue;
            }
            try {
                specs.push_back(ParseCommandFile(entry.path(), command_dir.trusted));
            } catch (const std::exception& error) {
                // One malformed command must not hide the rest from the picker.
                common::DefaultLogger().warn("command '{}' skipped in listing: {}",
                                             entry.path().string(), error.what());
            }
        }
    }

    out.reserve(specs.size());
    for (auto& spec : specs) out.push_back(MakeCommand(std::move(spec)));
    return out;
}

std::string FingerprintCommandDirs(const std::vector<CommandDir>& dirs) {
    std::string out;
    const auto stamp = [&out](const std::filesystem::path& path,
                              std::filesystem::file_time_type time, std::uintmax_t size) {
        out += path.string();
        out += '|';
        // Nanoseconds as a long long: libc++'s file_time_type counts in
        // __int128, which std::to_string has no overload for.
        out += std::to_string(static_cast<long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch())
                .count()));
        out += '|';
        out += std::to_string(size);
        out += '\n';
    };
    try {
        for (const auto& command_dir : dirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(command_dir.path, ec) || ec) continue;
            stamp(command_dir.path, std::filesystem::last_write_time(command_dir.path, ec), 0);
            for (const auto& entry : std::filesystem::directory_iterator(command_dir.path, ec)) {
                if (ec) break;
                if (entry.path().extension() != ".md") continue;
                std::error_code file_ec;
                const auto time = entry.last_write_time(file_ec);
                const auto size = entry.file_size(file_ec);
                if (!file_ec) stamp(entry.path(), time, size);
            }
        }
    } catch (const std::exception&) {
        // Allocation failure or a filesystem throw the error_code overloads
        // did not catch: whatever was gathered still identifies the state.
    }
    return out;
}

}  // namespace minimal::commands_md
