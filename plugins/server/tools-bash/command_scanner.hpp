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
    // One entry per `command` node the tree contains (top level and nested:
    // subshells, substitutions, do-groups), in tree order -- the trimmed text
    // of the whole simple command, leading `variable_assignment`s stripped and
    // `time`/`coproc` in the command slot demoted to what follows them. This
    // is what per-stage pattern matching runs over: "git *" should match the
    // stage `git status` of `git status | wc -l`, not the whole line.
    std::vector<std::string> stages;
    // Whether an `allow` pattern rule may act on `stages`. This is the PERMIT
    // direction of the old reject-the-metacharacters scanner: it does not gate
    // execution, only whether a config allow can skip the prompt, so failure
    // is always the safe direction (the Ask default stands). Ineligible when
    // the parse was not complete, no stages were found, or ANY stage writes to
    // a file -- an output redirect is a write the pattern layer has no rules
    // for (`git diff > /tmp/x` is not what "allow git diff" said). An inert
    // redirect stays eligible: fd dups (`2>&1`, `2>&-`) carry no path, and a
    // target of `/dev/null` writes nothing anyone wants a rule about.
    bool allow_eligible = true;

    bool complete = true;
};

ScanResult ScanCommand(std::string_view command);

}  // namespace minimal::tools_bash
