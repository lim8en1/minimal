#pragma once

#include <memory>

#include "search_settings.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools_search {

std::unique_ptr<tools::Tool> MakeGrepTool(SearchSettings settings);

}  // namespace minimal::tools_search
