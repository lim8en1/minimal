#pragma once

// opencode's providers.json shape -> core's, one entry at a time.

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::opencode_compat {

// Rewrites an opencode-shaped provider entry into core's shape. Native keys
// always win over the opencode spelling of the same thing; an entry with no
// opencode keys comes back unchanged.
nlohmann::json TranslateOpencodeProvider(const nlohmann::json& provider, const std::string& where);

// The api key for `provider_name` in an opencode auth.json document
// ({"<provider>": {"key"|"api_key": "..."}}), or nullopt.
std::optional<std::string> LookupOpencodeKey(const nlohmann::json& auth_doc,
                                             const std::string& provider_name);

}  // namespace minimal::opencode_compat
