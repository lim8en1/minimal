#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
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

// Reads an integer field of `parent`, clamped into [min, max]; falls back to
// `fallback` when `parent` is not an object, the field is absent, or the
// field is not an INTEGER json number. Deliberately `is_number_integer()`,
// not `is_number()`: `is_number()` accepts a float like 131072.9, which
// `get<int64_t>()` then silently truncates rather than rejecting -- a
// repeated mistake across plugin settings parsers this exists to end in one
// place. `min`/`max` bound a value before it reaches arithmetic that can
// overflow (a chrono duration conversion, a bit-shifted backoff delay);
// clamping here is cheaper than clamping at every such use site downstream
// and cannot be forgotten by a future one.
std::int64_t ReadClampedInt64(const nlohmann::json& parent, const char* key,
                              std::int64_t fallback, std::int64_t min, std::int64_t max);

// Strict schema for a config object: throws std::runtime_error naming `where`,
// the first key of `object` not in `known`, and the known set. Keys starting
// with "//" are comments and always allowed. A non-object is not checked (the
// caller's own type check names that). A misspelled or removed field must be a
// named error, not a silent ignore that surfaces later as a 401.
void RejectUnknownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> known,
                       const std::string& where);

}  // namespace minimal::config
