#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/message.hpp"

namespace minimal::session {

// llm::Message <-> JSON for the messages.content_json column.
//
// Decode is strict by default: a malformed field or a wrong-typed value throws
// std::runtime_error rather than dropping data -- those are corruption or a
// writer bug, and presenting the row as valid hides real damage.
//
// The ONE deliberate exception is an unknown block TYPE: it decodes to a
// readable placeholder TextBlock ("[unknown block type ...]") instead of
// throwing. A stored transcript from a newer binary would otherwise brick
// scrollback AND resume on every older binary -- one unrecognized block made
// every row it was in unreadable, an upgrade hazard for a public release with
// mixed versions. The placeholder text is the warning: it is visible to the
// client and travels to the model as part of the message.
nlohmann::json MessageToJson(const llm::Message& message);
llm::Message JsonToMessage(const nlohmann::json& doc);

}  // namespace minimal::session
