#pragma once

#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace minimal::config {

// A JSON object of string->string, as env/header maps are shaped in mcps.json
// and channels.json. `field` names the field in every error message. Throws
// std::runtime_error if `doc` is not an object, a key or value is not a
// string, or a key or value contains a CR or LF (both fields land in an
// outbound HTTP header or a child's environment, where a CR/LF is either
// header-injection or nonsense no consumer expects -- the KEY is screened too,
// because a key becomes the header NAME; config is the user's own file, so
// this rejects rather than silently mangling it).
std::unordered_map<std::string, std::string> ParseStringMap(const nlohmann::json& doc,
                                                             const char* field);

}  // namespace minimal::config
