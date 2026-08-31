#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minimal::tools_bash {

// Best-effort extraction of the command words a shell string will invoke, for
// the permission prompt's disclosure.
//
// This is NOT a security control and never rejects anything -- the string is
// handed to /bin/sh verbatim regardless of what happens here. Its only job is to
// answer "which programs can I name, so the operator can grant them for the
// session". Its failure mode is therefore "offer fewer grants", never "block the
// user's work". That inversion is deliberate: the predecessor of this file was a
// tokenizer that rejected shell metacharacters, which cost pipes and bought no
// safety (see ARCHITECTURE.md -- Bash).
//
// Parsing is `tree-sitter-bash` (vendored via FetchContent, see CMakeLists.txt):
// the grammar yields a `command_name` node at every command position -- inside
// if/while/for, function bodies, subshells, command substitutions -- which is
// the whole class the hand-rolled walker this replaced could only approximate.
//
// `complete` is false only when the PROGRAM ITSELF is not a literal in the
// string -- `$X args`, `eval "$CMD"`, `. script`. There is then nothing to
// name, and a grant for something unnamed would be meaningless. It is
// emphatically not a list of scary constructs: `$(...)` and backticks are a
// nested command context, not a blind spot, so `make -j$(nproc)` is complete
// and yields {make, nproc}. Treating substitution as blindness would strand a
// large slice of everyday commands in permanent re-prompting.
//
// THE POSTURE THAT MAKES A GRAMMAR SAFE HERE: tree-sitter is error-recovering
// -- it produces a tree for input it does not understand rather than refusing
// -- so the walker treats every ERROR and MISSING node in the parse as "I did
// not understand this" and sets complete = false. A hand-rolled walker cannot
// distinguish "I parsed this" from "I mis-parsed this", which is exactly how
// the reserved-word hole (below) once produced complete = true on a string it
// had misread. The error signal is the point of the grammar, not a bonus.
//
// THE INVARIANT THIS OWES ITS CALLER: a program that appears as a LITERAL in a
// command position is either named in `command_words` or `complete` is false.
// Never both missing. `complete` is what lets ToolExecutor satisfy the grant
// conjunction without a prompt, so a string this claims to have understood
// fully, while having silently dropped one of its programs, authorises more
// than the operator ever saw. Shell reserved words broke exactly this once:
// `then` and `if` were recorded AS programs, which consumed the command slot
// and demoted the real program to an argument, so `ls; if true; then rm -rf /;
// fi` disclosed only {ls} and reported itself complete.
//
// Known and accepted limit: this can only see SYNTACTIC invocations. Semantic
// indirection is invisible to any parser -- `find . | xargs rm` yields
// {find, xargs} and never names rm; `make` runs whatever the Makefile says. The
// disclosure is a lower bound and the prompt must say so. That is a DIFFERENT
// thing from the bug above, and the difference is what the operator was shown:
// granting xargs is a decision about xargs, taken knowingly. Nobody decides
// anything about a program that was never disclosed.
struct ScanResult {
    std::vector<std::string> command_words;  // in first-appearance order, deduplicated
    bool complete = true;
};

ScanResult ScanCommand(std::string_view command);

}  // namespace minimal::tools_bash
