#include "commands_md.hpp"

// The .md frontmatter parser and the opencode desugarer. Everything here runs
// at LOAD time; invocation only binds, runs sources, and renders (see
// commands_md.cpp).

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
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
std::string DesugarShellInjections(const std::string& body, std::vector<Source>& into,
                                   bool& injected_any) {
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
            source.name = "cmd" + std::to_string(++count);
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
        source.timeout_seconds = value;
    }

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
    bool injected_any = false;
    std::string desugared_body = DesugarShellInjections(parsed.body, spec.sources, injected_any);
    spec.body = DesugarPositionalTokens(desugared_body);
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
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > kMaxCommandFileBytes) {
        throw std::runtime_error("command file exceeds 1 MiB; refusing to load");
    }
    std::ifstream in(path);
    if (!in) throw std::runtime_error("command file not found: " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().size() > kMaxCommandFileBytes) {
        throw std::runtime_error("command file exceeds 1 MiB; refusing to load");
    }

    const std::string stem = path.stem().string();
    if (stem.empty() || stem.front() == '.') {
        throw std::runtime_error("command file: invalid name '" + stem + "'");
    }
    return DecodeSpec(stem, config::parse_frontmatter_markdown(buffer.str()), path, trusted);
}

std::vector<std::unique_ptr<commands::Command>> LoadCommands(
    const std::vector<CommandDir>& dirs, const SpecFilter& accept) {
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
                MdCommandSpec spec = ParseCommandFile(entry.path(), command_dir.trusted);
                if (accept && !accept(spec)) continue;
                specs.push_back(std::move(spec));
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

bool AcceptNonColliding(commands::CommandRegistry& registry, const MdCommandSpec& spec) {
    if (spec.trusted || registry.Find(spec.name) == nullptr) return true;
    common::DefaultLogger().warn(
        "command '{}' from project-local file {} skipped: a command with that name is "
        "already registered, and the existing one wins",
        spec.name, spec.source_file.string());
    return false;
}

}  // namespace minimal::commands_md
