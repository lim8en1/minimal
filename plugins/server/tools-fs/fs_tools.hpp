#pragma once

#include <memory>
#include <string>

#include "fs_settings.hpp"
#include "minimal/tools/file_access.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools_fs {

// All three declare path_confinement_supported() + ExtractPath() so ToolExecutor
// classifies out-of-workdir access centrally -- no tool re-implements that rule.
//
// Each still confines the path again itself before touching the filesystem. That
// is not redundancy: the executor resolves to *classify* (permission), the tool
// resolves to *operate* (I/O, with the correct must_exist), and a tool must never
// act on a path it did not resolve itself.

std::unique_ptr<tools::Tool> MakeReadTool(FsSettings settings);
std::unique_ptr<tools::Tool> MakeWriteTool(FsSettings settings);
std::unique_ptr<tools::Tool> MakeEditTool(FsSettings settings);

// Rendered by `read` and `edit view`. Exposed so the format -- including the
// truncation footer the model relies on to page -- is testable directly.
std::string RenderWindow(const tools::FileWindow& window);

}  // namespace minimal::tools_fs
