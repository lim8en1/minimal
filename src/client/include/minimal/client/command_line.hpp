#pragma once

// A typed `/name args` line and the project root commands scope to: runtime
// helpers shared by the TUI's command path and the headless -p flag. They
// used to live in cli_args, so state.cpp and the picker controller included
// the argv parser to split a prompt line.

#include <string>
#include <string_view>
#include <vector>

namespace minimal::client {

// A `/name args` prompt, split. Present only when the prompt was one.
struct CommandInvocationText {
    std::string name;
    std::vector<std::string> positional;
    std::string raw;
};

// Splits "/name arg1 arg2" into its parts. Client-side because this is the side
// holding the text a human typed; the wire carries the result, so the server
// never re-splits a command line. The prompt editor's `/` path uses the same
// split the headless flag does -- one parser, not two opinions.
CommandInvocationText SplitCommand(std::string_view text);

// The project root scoping every session create/list call. Never throws: a
// deleted cwd (the project dir removed while the client is running) would
// otherwise abort with whatever shape std::filesystem::current_path() throws
// in. Falls back to an empty string, which every call site already treats as
// "no scope" (the same shape --all-dirs uses).
std::string CurrentProjectRootOrEmpty();

}  // namespace minimal::client
