#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/message.hpp"

namespace minimal::session {

// llm::Message <-> JSON for the messages.content_json column.
//
// The codec is strict on decode: an unknown block type or a malformed field
// throws std::runtime_error rather than dropping data. A row that cannot be
// decoded means corruption or a database written by code this binary does not
// understand -- both are things the operator must see, not silently survive.
nlohmann::json MessageToJson(const llm::Message& message);
llm::Message JsonToMessage(const nlohmann::json& doc);

}  // namespace minimal::session
