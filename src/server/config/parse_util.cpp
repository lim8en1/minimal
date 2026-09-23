#include "minimal/config/parse_util.hpp"

#include <stdexcept>
#include <utility>

namespace minimal::config {

std::unordered_map<std::string, std::string> ParseStringMap(const nlohmann::json& doc,
                                                             const char* field) {
    std::unordered_map<std::string, std::string> out;
    if (!doc.is_object()) {
        throw std::runtime_error(std::string(field) + " must be an object of strings");
    }
    for (const auto& [key, value] : doc.items()) {
        if (!value.is_string()) {
            throw std::runtime_error(std::string(field) + " value for '" + key + "' must be a string");
        }
        // Keys are screened too, not just values: a key can become a header
        // name (headers, env) downstream, and a CR/LF in it would be a
        // header-injection seam the value check alone does not close.
        if (key.find('\r') != std::string::npos || key.find('\n') != std::string::npos) {
            throw std::runtime_error(std::string(field) + " key '" + key +
                                     "' contains a CR or LF, which is not allowed");
        }
        std::string parsed = value.get<std::string>();
        if (parsed.find('\r') != std::string::npos || parsed.find('\n') != std::string::npos) {
            throw std::runtime_error(std::string(field) + " value for '" + key +
                                     "' contains a CR or LF, which is not allowed");
        }
        out.emplace(key, std::move(parsed));
    }
    return out;
}

std::int64_t ReadClampedInt64(const nlohmann::json& parent, const char* key,
                              std::int64_t fallback, std::int64_t min, std::int64_t max) {
    if (!parent.is_object()) return fallback;
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_number_integer()) return fallback;
    const std::int64_t value = it->get<std::int64_t>();
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

}  // namespace minimal::config
