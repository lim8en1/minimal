#pragma once

// The `ask_user` tool: ask the human 1..4 structured questions and return
// their answers. See plugin.cpp for the posture rationale and ask.hpp for
// the data shapes.

#include "minimal/tools/tool.hpp"

namespace minimal::tool_ask {

std::unique_ptr<tools::Tool> MakeAskTool();

}  // namespace minimal::tool_ask
