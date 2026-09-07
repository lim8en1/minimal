#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace minimal::common {

// Returns the file's full contents, or an empty string if it cannot be
// opened. Callers that need to distinguish "missing" from "empty" must not
// use this helper.
//
// Throws std::runtime_error when the file exceeds `max_bytes` (the default
// cap is generous -- the callers read prompts and small JSON docs, not data
// files) or when a genuine I/O error (badbit) occurs mid-read: a silently
// truncated string is worse than a loud failure for every current caller.
// eofbit/failbit are NOT errors -- an rdbuf insertion sets them for an
// empty file.
std::string ReadFileOrEmpty(const std::filesystem::path& path,
                            std::size_t max_bytes = 64u << 20);

}  // namespace minimal::common
