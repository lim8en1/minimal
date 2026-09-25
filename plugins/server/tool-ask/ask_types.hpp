#pragma once

// The ask's own data shapes and their JSON wire form -- the contract between
// this plugin and the client's `ask-view` prompt renderer. Core carries both
// JSON sides opaquely (minimal/tools/prompt.hpp); nothing outside the plugin
// pair reads them.
//
// Request payload (prompt kind "ask"):
//   {"questions": [{"question": str, "header": str, "multiple": bool,
//                   "allow_other": bool,
//                   "options": [{"label": str, "description": str}]}]}
// Answer:
//   {"answers": [{"selected": [label...], "other": str}]}   (index-aligned)

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::tool_ask {

inline constexpr const char* kPromptKind = "ask";

struct AskOption {
    std::string label;             // what the model receives back verbatim
    std::string description = {};  // human-facing elaboration; may be empty
};

struct AskQuestion {
    std::string question;
    std::string header;            // short human-facing label; may be empty
    std::vector<AskOption> options;
    bool multiple = false;         // checkbox semantics instead of a single pick
    bool allow_other = true;       // the client offers a free-text answer
};

struct AskRequest {
    std::vector<AskQuestion> questions;  // the tool validates 1..4
};

// One slot per question, index-aligned. A slot with neither field set is
// "question not answered".
struct AskSlot {
    std::vector<std::string> selected;
    std::string other;
};

nlohmann::json ToPayload(const AskRequest& request);

// Reads a client answer against the request it answers, fail-closed per slot:
// a slot naming a label the question did not OFFER, two picks on a single-
// select question, free text where allow_other is false, or free text mixed
// with a selection becomes an empty ("not answered") slot -- never an
// invented answer. Always returns one slot per question.
std::vector<AskSlot> ReadAnswer(const AskRequest& request, const nlohmann::json& answer);

}  // namespace minimal::tool_ask
