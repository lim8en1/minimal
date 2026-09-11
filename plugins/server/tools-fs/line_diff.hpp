#pragma once

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::tools_fs {

// Splits on '\n'. A trailing newline does not produce an empty last line.
std::vector<std::string_view> SplitLines(std::string_view text);

// A `diff` display payload (docs/EXTENSIBILITY.md, "Tool-result display
// payloads") for old_text -> new_text: unified hunks with 3 context lines.
// Null when either side looks binary or the change is too large to show
// usefully; the client then falls back to the result text.
nlohmann::json DiffDisplay(std::string_view path, std::string_view old_text,
                           std::string_view new_text, bool created);

}  // namespace minimal::tools_fs
