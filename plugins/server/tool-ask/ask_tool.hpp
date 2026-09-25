#pragma once

// The `ask_user` tool: ask the human 1..4 structured questions and return
// their answers. See plugin.cpp for the posture rationale and ask_types.hpp
// for the data shapes and their wire form.

#include "minimal/tools/tool.hpp"

namespace minimal::tool_ask {

std::unique_ptr<tools::Tool> MakeAskTool();

}  // namespace minimal::tool_ask
