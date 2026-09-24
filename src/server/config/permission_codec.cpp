#include "minimal/config/permission_codec.hpp"

#include <stdexcept>

namespace minimal::config {

tools::PermissionMode ParsePermissionMode(const std::string& value) {
    if (auto mode = tools::mode_from_name(value)) return *mode;
    throw std::runtime_error("invalid permission mode: '" + value + "' (expected allow|ask|deny)");
}

tools::PermissionRunMode ParsePermissionRunMode(const std::string& value) {
    if (auto mode = tools::run_mode_from_name(value)) return *mode;
    throw std::runtime_error("invalid permission run mode: '" + value +
                             "' (expected normal|bypass|allow-only)");
}

tools::PatternPrecedence ParsePatternPrecedence(const std::string& value) {
    if (auto precedence = tools::precedence_from_name(value)) return *precedence;
    throw std::runtime_error("invalid pattern_precedence: '" + value +
                             "' (expected restrictive|specific)");
}

std::string ToString(tools::PermissionRunMode mode) {
    return std::string(tools::run_mode_name(mode));
}

}  // namespace minimal::config
