#pragma once

// commands-md -- markdown-defined /-commands, as one plugin.
//
// Every commands/*.md file becomes a PromptShorthandCommand: the file stem is
// the command name, the frontmatter declares parameters and data sources, and
// the body is an INERT render template. The design rule that bounds the whole
// feature: the body never executes anything.
// All execution lives in declared `sources`, evaluated in declaration order,
// flat and independent; the body is a single-pass render of already-evaluated
// values, and inserted values are never re-scanned.
//
// opencode syntax is desugared at load, not run through a second engine:
// $ARGUMENTS -> {args}, $1..$9 -> positional entries, and each !`cmd` becomes
// an anonymous `sh` source. A file mixing desugared constructs with native
// sources works (every construct is just another flat source) and is
// lint-warned.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/commands/command.hpp"
#include "minimal/commands/command_registry.hpp"

namespace minimal::commands_md {

// What a source's stdout is: raw text usable as {name}, or JSON whose declared
// fields are addressable as {output:name.field}.
enum class SourceOutput { kText, kJson };

// output_cap behavior: pause and ask the operator (with a bytes/4 token
// estimate), or fail outright. Truncation is deliberately absent -- half a
// prompt costs the same as a whole one and does half the job.
enum class CapAction { Confirm, Fail };

struct Source {
    std::string name;
    bool use_shell = false;         // true: `sh` string; false: `run` argv
    std::vector<std::string> argv;  // run form; {name} substituted per entry
    std::string script;             // sh form (/bin/sh -c)
    SourceOutput output = SourceOutput::kText;
    std::vector<std::string> fields;   // expected JSON fields when kJson
    int timeout_seconds = 600;         // 0 = infinite
    std::size_t cap_tokens = 50000;    // output cap, token estimate (bytes/4)
    CapAction on_exceed = CapAction::Confirm;
};

struct MdCommandSpec {
    std::string name;                // file stem
    std::string summary;             // frontmatter description
    std::vector<std::string> args;   // required, bound to positionals in order
    std::vector<std::string> optional;
    std::vector<Source> sources;
    std::string body;                // inert template; empty = the sole text source's stdout
    std::filesystem::path source_file;  // diagnostics only
    // False for a command file discovered from a PROJECT-LOCAL directory (the
    // session's working tree, walked up to the git root -- see ProjectDirs):
    // that is content the working directory supplied, not something the
    // operator configured, the same distinction agent-preset resolution makes
    // for `permission:`. An untrusted command's sources are disclosed and
    // confirmed before ANY of them runs (MdCommand::Execute) -- a repo-shipped
    // commands/*.md must not execute a generator program silently the first
    // time someone types its name. A GLOBAL-config command (the operator's own
    // config_dir/commands/) stays trusted and runs without that extra prompt,
    // matching every other command in the harness.
    bool trusted = true;
    // PER-TURN overrides (opencode `model:`/`agent:` frontmatter): the turn
    // this command submits runs on the given model and/or agent preset; the
    // session's sticky choices are untouched. The model is kept in raw
    // "<provider>/<model>" form (SplitModelRef-shaped; malformed refs fail
    // the turn at pop with a named error). Empty = unset.
    std::string model;
    std::string agent;
};

// Parses one .md file into a spec. Throws std::runtime_error with the reason on
// any structural problem -- a broken command file must not load as a command
// that fails at invocation with a mystery.
MdCommandSpec ParseCommandFile(const std::filesystem::path& path, bool trusted);

// Builds the command object over a parsed spec.
std::unique_ptr<commands::Command> MakeCommand(MdCommandSpec spec);

// One directory to scan, tagged with whether files found there are trusted
// (the operator's own global config) or not (project-local, working-tree
// supplied). See MdCommandSpec::trusted.
struct CommandDir {
    std::filesystem::path path;
    bool trusted = true;
};

// Optional per-spec veto, called after a file parses and before its command
// object is built: returning false skips the file (the caller warns). The
// plugin's registration hook uses AcceptNonColliding here.
using SpecFilter = std::function<bool(const MdCommandSpec&)>;

// Loads every *.md under the given directories, nearest dir winning a stem
// collision (project shadows global -- `dirs` must therefore list project
// directories BEFORE the global one). Malformed files are warned and skipped:
// one broken command must not hide the rest from the picker. `accept`, when
// set, vetoes individual specs the same way.
std::vector<std::unique_ptr<commands::Command>> LoadCommands(
    const std::vector<CommandDir>& dirs, const SpecFilter& accept = nullptr);

// Registration-time veto for the plugin's hook: false only when the spec is
// UNTRUSTED (project-local -- content a cloned working tree supplied) AND its
// command name is already registered. A duplicate surface name is otherwise a
// FATAL startup collision, so repo-shipped commands/*.md could refuse to let
// the server start at all; skipping lets the earlier registration win. A
// TRUSTED (global config) file always passes -- the operator wrote it, and a
// collision there stays the loud fatal it always was.
bool AcceptNonColliding(commands::CommandRegistry& registry, const MdCommandSpec& spec);

}  // namespace minimal::commands_md
