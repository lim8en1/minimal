#include "minimal/mcp/naming.hpp"

#include <algorithm>

namespace minimal::mcp {

namespace {

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
    // Trailing '_' is rejected too: McpServerOf splits at the FIRST "__", so
    // server "a" + tool "_x" and server "a_" + tool "x" would both mangle to
    // "mcp__a___x". With "a_" invalid, that collision class cannot exist.
    return !server_name.empty() && server_name.find(kMcpSeparator) == std::string_view::npos &&
           server_name.back() != '_' && !HasDisallowedCharacter(server_name);
}

bool IsValidMcpToolName(std::string_view tool_name) {
    return IsValidMcpServerName(tool_name);
}

std::string McpToolName(std::string_view server_name, std::string_view tool_name) {
    std::string out;
    out.reserve(kMcpPrefix.size() + server_name.size() + kMcpSeparator.size() + tool_name.size());
    out.append(kMcpPrefix);
    out.append(server_name);
    out.append(kMcpSeparator);
    out.append(tool_name);
    return out;
}

std::string McpGroup(std::string_view server_name) {
    std::string out;
    out.reserve(kMcpPrefix.size() + server_name.size() + kMcpSeparator.size());
    out.append(kMcpPrefix);
    out.append(server_name);
    out.append(kMcpSeparator);
    return out;
}

std::string McpServerOf(std::string_view tool_name) {
    if (tool_name.substr(0, kMcpPrefix.size()) != kMcpPrefix) return {};
    const auto rest = tool_name.substr(kMcpPrefix.size());
    const auto sep = rest.find(kMcpSeparator);
    if (sep == std::string_view::npos) return {};
    return std::string(rest.substr(0, sep));
}

}  // namespace minimal::mcp
