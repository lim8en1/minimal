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

}  // namespace minimal::config
