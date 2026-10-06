#pragma once

#include <memory>

#include "minimal/common/live_value.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/tools/tool.hpp"
#include "web_settings.hpp"

namespace minimal::tools_web {

// Reads `live` once per call: a reconfigure swaps it under a running server.
std::unique_ptr<tools::Tool> MakeWebSearchTool(
    std::shared_ptr<const common::LiveValue<WebSettings>> live, net::HttpClient http);
// A fixed configuration (tests).
std::unique_ptr<tools::Tool> MakeWebSearchTool(WebSettings settings, net::HttpClient http);

}  // namespace minimal::tools_web
