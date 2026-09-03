#pragma once

#include <filesystem>
#include <string>

namespace minimal::common {

// Returns the file's full contents, or an empty string if it cannot be
// opened. Callers that need to distinguish "missing" from "empty" must not
// use this helper.
std::string ReadFileOrEmpty(const std::filesystem::path& path);

}  // namespace minimal::common
