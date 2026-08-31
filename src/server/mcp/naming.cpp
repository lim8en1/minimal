#include "minimal/mcp/naming.hpp"

#include <algorithm>

namespace minimal::mcp {

namespace {

constexpr std::string_view kPrefix = "mcp__";
constexpr std::string_view kSeparator = "__";

bool HasDisallowedCharacter(std::string_view name) {
    return std::any_of(name.begin(), name.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        // Whitespace, other C0 controls, and DEL: all corrupt a name the model
        // must read and call back verbatim. (0x20 is space itself.)
        return u <= 0x20 || u == 0x7f;
    });
}

}  // namespace

bool IsValidMcpServerName(std::string_view server_name) {
    return !server_name.empty() && server_name.find(kSeparator) == std::string_view::npos &&
           !HasDisallowedCharacter(server_name);
}

bool IsValidMcpToolName(std::string_view tool_name) {
    return IsValidMcpServerName(tool_name);
}

std::string McpToolName(std::string_view server_name, std::string_view tool_name) {
    std::string out;
    out.reserve(kPrefix.size() + server_name.size() + kSeparator.size() + tool_name.size());
    out.append(kPrefix);
    out.append(server_name);
    out.append(kSeparator);
    out.append(tool_name);
    return out;
}

}  // namespace minimal::mcp
