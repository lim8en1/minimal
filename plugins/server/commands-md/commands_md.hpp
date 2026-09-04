#pragma once

// commands-md -- markdown-defined /-commands, as one plugin.
//
// Every commands/*.md file becomes a PromptShorthandCommand: the file stem is
// the command name, the frontmatter declares parameters and data sources, and
// the body is an INERT render template. The design rule that bounds the whole
// feature (docs/OPENCODE_COMPAT.md, Phase 4): the body never executes anything.
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
};

// Parses one .md file into a spec. Throws std::runtime_error with the reason on
// any structural problem -- a broken command file must not load as a command
// that fails at invocation with a mystery.
MdCommandSpec ParseCommandFile(const std::filesystem::path& path);

// Builds the command object over a parsed spec.
std::unique_ptr<commands::Command> MakeCommand(MdCommandSpec spec);

// Loads every *.md under the given directories, nearest dir winning a stem
// collision (project shadows global). Malformed files are warned and skipped:
// one broken command must not hide the rest from the picker.
std::vector<std::unique_ptr<commands::Command>> LoadCommands(
    const std::vector<std::filesystem::path>& dirs);

}  // namespace minimal::commands_md
