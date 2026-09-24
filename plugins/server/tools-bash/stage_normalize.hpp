#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minimal::tools_bash {

// Extra spellings of one command stage, for ask/deny pattern matching ONLY.
//
// Stage text is the command as written, so `"rm *": deny` missed `/bin/rm x`,
// `'rm' x`, `\rm x`, `command rm x`, `sudo rm x` and `xargs rm`. This returns
// the forms a rule author means by "rm": the words unquoted, the program
// named by its basename, and each wrapper program (sudo, env, command,
// xargs, timeout, nice, ...) peeled off with its own flags. Only spellings
// that DIFFER from the stage are returned.
//
// Restrictive-only by construction: the policy folds these into ask/deny
// matching and never into the allow conjunction, so a normalization mistake
// can only cause an extra prompt or denial -- never an allow. That is also
// why the tokenizer is a light one (quotes and backslashes, whitespace
// splitting) rather than a shell: over-matching is the safe failure.
//
// Still a guardrail, not a boundary: `sh -c 'rm x'`, `$(echo rm) x` and
// `find -exec rm` are out of reach of any string rule.
std::vector<std::string> NormalizedStageVariants(std::string_view stage);

}  // namespace minimal::tools_bash
