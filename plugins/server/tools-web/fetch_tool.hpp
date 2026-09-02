#pragma once

#include <memory>

#include "minimal/llm/http_client.hpp"
#include "minimal/tools/tool.hpp"
#include "web_settings.hpp"

namespace minimal::tools_web {

std::unique_ptr<tools::Tool> MakeWebFetchTool(WebSettings settings, llm::HttpClient http);

}  // namespace minimal::tools_web
