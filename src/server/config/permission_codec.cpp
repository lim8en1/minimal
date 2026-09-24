#include "minimal/config/permission_codec.hpp"

#include <stdexcept>

namespace minimal::config {

tools::PermissionMode ParsePermissionMode(const std::string& value) {
    if (value == "allow") return tools::PermissionMode::AlwaysAllow;
    if (value == "ask") return tools::PermissionMode::Ask;
    if (value == "deny") return tools::PermissionMode::AlwaysDeny;
    throw std::runtime_error("invalid permission mode: '" + value + "' (expected allow|ask|deny)");
}

tools::PermissionRunMode ParsePermissionRunMode(const std::string& value) {
    if (value == "normal") return tools::PermissionRunMode::Normal;
    if (value == "bypass") return tools::PermissionRunMode::Bypass;
    if (value == "allow-only") return tools::PermissionRunMode::AllowOnly;
    throw std::runtime_error("invalid permission run mode: '" + value + "' (expected normal|bypass|allow-only)");
}

tools::PatternPrecedence ParsePatternPrecedence(const std::string& value) {
    if (value == "restrictive") return tools::PatternPrecedence::Restrictive;
    if (value == "specific") return tools::PatternPrecedence::Specific;
    throw std::runtime_error("invalid pattern_precedence: '" + value +
                             "' (expected restrictive|specific)");
}

std::string ToString(tools::PermissionRunMode mode) {
    switch (mode) {
        case tools::PermissionRunMode::AllowOnly: return "allow-only";
        case tools::PermissionRunMode::Bypass: return "bypass";
        case tools::PermissionRunMode::Normal: return "normal";
    }
    return "normal";
}

}  // namespace minimal::config
