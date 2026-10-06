#pragma once

#include <memory>

#include "search_settings.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools_search {

std::unique_ptr<tools::Tool> MakeGlobTool(LiveSearchSettings live);
std::unique_ptr<tools::Tool> MakeGlobTool(SearchSettings settings);  // fixed (tests)

}  // namespace minimal::tools_search
