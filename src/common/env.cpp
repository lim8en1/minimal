#include "minimal/common/env.hpp"

#include <cstdlib>

namespace minimal::common {

std::optional<std::string> LookupEnv(std::string_view name) {
    const char* v = std::getenv(std::string(name).c_str());
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

}  // namespace minimal::common
