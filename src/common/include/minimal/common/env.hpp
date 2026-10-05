#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace minimal::common {

// getenv, but treats an empty value as unset -- an exported-but-empty env var
// (XDG_CONFIG_HOME, an MCP secret referenced via ${VAR}, an "env:NAME" key
// reference) is common and must not be treated as a resolved value. Every
// env-var lookup in the tree goes through this rather than calling getenv.
std::optional<std::string> LookupEnv(std::string_view name);

}  // namespace minimal::common
