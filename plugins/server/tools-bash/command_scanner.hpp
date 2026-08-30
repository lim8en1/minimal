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
// `complete` is false only when the PROGRAM ITSELF is not a literal in the
// string -- `$X args`, `eval "$CMD"`, `. script`. There is then nothing to name,
// and a grant for something unnamed would be meaningless. It is emphatically not
// a list of scary constructs: `$(...)` and backticks are a nested command
// context, not a blind spot, so `make -j$(nproc)` is complete and yields
// {make, nproc}. Treating substitution as blindness would strand a large slice
// of everyday commands in permanent re-prompting.
//
// Known and accepted limit: this can only see SYNTACTIC invocations. Semantic
// indirection is invisible to any parser -- `find . | xargs rm` yields
// {find, xargs} and never names rm; `make` runs whatever the Makefile says. The
// disclosure is a lower bound and the prompt must say so.
struct ScanResult {
    std::vector<std::string> command_words;  // in first-appearance order, deduplicated
    bool complete = true;
};

ScanResult ScanCommand(std::string_view command);

}  // namespace minimal::tools_bash
