#pragma once

#include <memory>

#include "minimal/net/http_client.hpp"
#include "minimal/tools/tool.hpp"
#include "web_settings.hpp"

namespace minimal::tools_web {

std::unique_ptr<tools::Tool> MakeWebSearchTool(WebSettings settings, net::HttpClient http);

}  // namespace minimal::tools_web
