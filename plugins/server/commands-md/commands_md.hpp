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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/commands/command.hpp"
#include "policy.hpp"

namespace minimal::commands_md {

// What a source's stdout is: raw text usable as {name}, or JSON whose declared
// fields are addressable as {output:name.field}.
enum class SourceOutput { kText, kJson };

// output_cap behavior: pause and ask the operator (with a token estimate,
// agent::kEstimatedBytesPerToken), or fail outright. Truncation is
// deliberately absent -- half a prompt costs the same as a whole one and does
// half the job.
enum class CapAction { Confirm, Fail };

struct Source {
    std::string name;
    bool use_shell = false;         // true: `sh` string; false: `run` argv
    std::vector<std::string> argv;  // run form; {name} substituted per entry
    std::string script;             // sh form (/bin/sh -c)
    SourceOutput output = SourceOutput::kText;
    std::vector<std::string> fields;   // expected JSON fields when kJson
    int timeout_seconds = kDefaultTimeoutSeconds;  // 0 = no deadline
    std::size_t cap_tokens = kDefaultCapTokens;    // output cap, estimated tokens
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

// Loads every *.md under the given directories, nearest dir winning a stem
// collision (project shadows global -- `dirs` must therefore list project
// directories BEFORE the global one). Malformed files are warned and skipped:
// one broken command must not hide the rest from the picker.
//
// No registry-collision veto is needed: the global hook loads only trusted
// files (a collision there is the operator's own, and stays fatal), and the
// project hook loads into a fresh per-root registry that nothing else shares.
// Whether a project command may shadow a GLOBAL name is decided at lookup
// time instead (commands::ProjectMayShadow).
std::vector<std::unique_ptr<commands::Command>> LoadCommands(const std::vector<CommandDir>& dirs);

// What LoadCommands(dirs) would read, summarized at stat level: each existing
// dir's mtime (catches a file added, removed or renamed) and each *.md file's
// mtime and size (catches an edit). Never parses; never throws. Backs the
// plugin's minimal_plugin_project_commands_fingerprint hook.
std::string FingerprintCommandDirs(const std::vector<CommandDir>& dirs);

}  // namespace minimal::commands_md
