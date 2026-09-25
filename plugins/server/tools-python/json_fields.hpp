#pragma once

// Non-throwing JSON field reads. nlohmann's value() throws type_error when a
// key is present with the wrong type -- fine for JSON we authored, wrong for
// MODEL input (`{"clean": "yes"}` is an ordinary mistake to report) and for
// protocol lines from the driver process (executed code can corrupt them).
// A present-but-wrong-typed field reads as absent, same as a missing one.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::tools_python {

inline std::string StringField(const nlohmann::json& body, const char* key,
                               std::string fallback = {}) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

inline bool BoolField(const nlohmann::json& body, const char* key, bool fallback = false) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

inline std::uint64_t UintField(const nlohmann::json& body, const char* key,
                               std::uint64_t fallback = 0) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_number_unsigned()) return fallback;
    return it->get<std::uint64_t>();
}

}  // namespace minimal::tools_python
